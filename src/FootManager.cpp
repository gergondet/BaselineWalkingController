#include <limits>

#include <mc_filter/utils/clamp.h>
#include <mc_rtc/gui/ArrayInput.h>
#include <mc_rtc/gui/Checkbox.h>
#include <mc_rtc/gui/Label.h>
#include <mc_rtc/gui/NumberInput.h>
#include <mc_rtc/gui/Polygon.h>
#include <mc_tasks/OrientationTask.h>

#include <BaselineWalkingController/BaselineWalkingController.h>
#include <BaselineWalkingController/FootManager.h>
#include <BaselineWalkingController/tasks/FirstOrderImpedanceTask.h>
#include <BaselineWalkingController/trajectory/CubicSpline.h>
#include <BaselineWalkingController/wrench/Contact.h>

using namespace BWC;

void FootManager::Configuration::load(const mc_rtc::Configuration & mcRtcConfig)
{
  mcRtcConfig("name", name);
  mcRtcConfig("footstepDuration", footstepDuration);
  mcRtcConfig("doubleSupportRatio", doubleSupportRatio);
  if(mcRtcConfig.has("midToFootTranss"))
  {
    for(const auto & foot : Feet::Both)
    {
      mcRtcConfig("midToFootTranss")(std::to_string(foot), midToFootTranss.at(foot));
    }
  }
  mcRtcConfig("zmpHorizon", zmpHorizon);
  mcRtcConfig("zmpOffset", zmpOffset);
  mcRtcConfig("overwriteLandingPose", overwriteLandingPose);
  mcRtcConfig("stopSwingTrajForTouchDownFoot", stopSwingTrajForTouchDownFoot);
  mcRtcConfig("keepSupportFootPoseForTouchDownFoot", keepSupportFootPoseForTouchDownFoot);
  mcRtcConfig("enableWrenchDistForTouchDownFoot", enableWrenchDistForTouchDownFoot);
  mcRtcConfig("fricCoeff", fricCoeff);
  mcRtcConfig("touchDownRemainingDuration", touchDownRemainingDuration);
  mcRtcConfig("touchDownPosError", touchDownPosError);
  mcRtcConfig("touchDownForceZ", touchDownForceZ);
  if(mcRtcConfig.has("impedanceGains"))
  {
    mcRtcConfig("impedanceGains")("singleSupport", impGains.at("singleSupport"));
    mcRtcConfig("impedanceGains")("doubleSupport", impGains.at("doubleSupport"));
    mcRtcConfig("impedanceGains")("swing", impGains.at("swing"));
  }
}

FootManager::FootManager(BaselineWalkingController * ctlPtr, const mc_rtc::Configuration & mcRtcConfig)
: ctlPtr_(ctlPtr), zmpFunc_(std::make_shared<CubicInterpolator<Eigen::Vector3d>>()),
  groundPosZFunc_(std::make_shared<CubicInterpolator<double>>()),
  swingPosFunc_(std::make_shared<PiecewiseFunc<Eigen::Vector3d>>()),
  swingRotFunc_(std::make_shared<CubicInterpolator<Eigen::Matrix3d, Eigen::Vector3d>>()),
  baseYawFunc_(std::make_shared<CubicInterpolator<Eigen::Matrix3d, Eigen::Vector3d>>())
{
  config_.load(mcRtcConfig);
}

void FootManager::reset()
{
  footstepQueue_.clear();
  prevFootstep_.reset();

  for(const auto & foot : Feet::Both)
  {
    targetFootPoses_.emplace(foot, ctl().robot().surfacePose(surfaceName(foot)));
    targetFootVels_.emplace(foot, sva::MotionVecd::Zero());
    targetFootAccels_.emplace(foot, sva::MotionVecd::Zero());
  }
  lastDoubleSupportFootPoses_ = targetFootPoses_;

  supportPhase_ = SupportPhase::DoubleSupport;

  Eigen::Vector3d targetZmp = calcZmpWithOffset(targetFootPoses_);
  zmpFunc_->clearPoints();
  zmpFunc_->appendPoint(std::make_pair(ctl().t(), targetZmp));
  zmpFunc_->appendPoint(std::make_pair(ctl().t() + config_.zmpHorizon, targetZmp));
  zmpFunc_->calcCoeff();

  double refGroundPosZ =
      0.5 * (targetFootPoses_.at(Foot::Left).translation().z() + targetFootPoses_.at(Foot::Right).translation().z());
  groundPosZFunc_->clearPoints();
  groundPosZFunc_->appendPoint(std::make_pair(ctl().t(), refGroundPosZ));
  groundPosZFunc_->appendPoint(std::make_pair(ctl().t() + config_.zmpHorizon, refGroundPosZ));
  groundPosZFunc_->calcCoeff();

  contactFootPosesList_.emplace(ctl().t(), targetFootPoses_);

  swingFootstep_ = nullptr;

  swingPosFunc_->clearFuncs();
  swingRotFunc_->clearPoints();

  baseYawFunc_->clearPoints();

  touchDown_ = false;

  for(const auto & foot : Feet::Both)
  {
    impGainTypes_.emplace(foot, "doubleSupport");
  }

  requireImpGainUpdate_ = true;

  overwriteLandingPosLowPass_.dt(ctl().solver().dt());
  overwriteLandingPosLowPass_.reset(Eigen::Vector3d::Zero());
}

void FootManager::update()
{
  updateFootTraj();
  updateZmpTraj();
}

void FootManager::stop()
{
  removeFromGUI(*ctl().gui());
  removeFromLogger(ctl().logger());
}

void FootManager::addToGUI(mc_rtc::gui::StateBuilder & gui)
{
  gui.addElement({ctl().name(), config_.name, "Status"},
                 mc_rtc::gui::Label("supportPhase", [this]() { return std::to_string(supportPhase_); }),
                 mc_rtc::gui::Label("footstepQueueSize", [this]() { return std::to_string(footstepQueue_.size()); }));
  gui.addElement({ctl().name(), config_.name, "Status"}, mc_rtc::gui::ElementsStacking::Horizontal,
                 mc_rtc::gui::Label("LeftFootSurface", [this]() { return surfaceName(Foot::Left); }),
                 mc_rtc::gui::Label("RightFootSurface", [this]() { return surfaceName(Foot::Right); }));
  gui.addElement({ctl().name(), config_.name, "Status"}, mc_rtc::gui::ElementsStacking::Horizontal,
                 mc_rtc::gui::Label("LeftImpGainType", [this]() { return impGainTypes_.at(Foot::Left); }),
                 mc_rtc::gui::Label("RightImpGainType", [this]() { return impGainTypes_.at(Foot::Right); }));

  gui.addElement(
      {ctl().name(), config_.name, "Config"},
      mc_rtc::gui::NumberInput(
          "footstepDuration", [this]() { return config_.footstepDuration; },
          [this](double v) { config_.footstepDuration = v; }),
      mc_rtc::gui::NumberInput(
          "doubleSupportRatio", [this]() { return config_.doubleSupportRatio; },
          [this](double v) { config_.doubleSupportRatio = v; }),
      mc_rtc::gui::ArrayInput(
          "zmpOffset", {"x", "y", "z"}, [this]() -> const Eigen::Vector3d & { return config_.zmpOffset; },
          [this](const Eigen::Vector3d & v) { config_.zmpOffset = v; }),
      mc_rtc::gui::Checkbox(
          "overwriteLandingPose", [this]() { return config_.overwriteLandingPose; },
          [this]() { config_.overwriteLandingPose = !config_.overwriteLandingPose; }),
      mc_rtc::gui::Checkbox(
          "stopSwingTrajForTouchDownFoot", [this]() { return config_.stopSwingTrajForTouchDownFoot; },
          [this]() { config_.stopSwingTrajForTouchDownFoot = !config_.stopSwingTrajForTouchDownFoot; }),
      mc_rtc::gui::Checkbox(
          "keepSupportFootPoseForTouchDownFoot", [this]() { return config_.keepSupportFootPoseForTouchDownFoot; },
          [this]() { config_.keepSupportFootPoseForTouchDownFoot = !config_.keepSupportFootPoseForTouchDownFoot; }),
      mc_rtc::gui::Checkbox(
          "enableWrenchDistForTouchDownFoot", [this]() { return config_.enableWrenchDistForTouchDownFoot; },
          [this]() { config_.enableWrenchDistForTouchDownFoot = !config_.enableWrenchDistForTouchDownFoot; }),
      mc_rtc::gui::NumberInput(
          "fricCoeff", [this]() { return config_.fricCoeff; }, [this](double v) { config_.fricCoeff = v; }),
      mc_rtc::gui::NumberInput(
          "touchDownRemainingDuration", [this]() { return config_.touchDownRemainingDuration; },
          [this](double v) { config_.touchDownRemainingDuration = v; }),
      mc_rtc::gui::NumberInput(
          "touchDownPosError", [this]() { return config_.touchDownPosError; },
          [this](double v) { config_.touchDownPosError = v; }),
      mc_rtc::gui::NumberInput(
          "touchDownForceZ", [this]() { return config_.touchDownForceZ; },
          [this](double v) { config_.touchDownForceZ = v; }));

  for(const auto & impGainKV : config_.impGains)
  {
    const auto & impGainType = impGainKV.first;
    gui.addElement({ctl().name(), config_.name, "ImpedanceGains", impGainType},
                   mc_rtc::gui::ArrayInput(
                       "Damper", {"cx", "cy", "cz", "fx", "fy", "fz"},
                       [this, impGainType]() -> const sva::ImpedanceVecd & {
                         return config_.impGains.at(impGainType).damper().vec();
                       },
                       [this, impGainType](const Eigen::Vector6d & v) {
                         config_.impGains.at(impGainType).damper().vec(v);
                         requireImpGainUpdate_ = true;
                       }));
    gui.addElement({ctl().name(), config_.name, "ImpedanceGains", impGainType},
                   mc_rtc::gui::ArrayInput(
                       "Spring", {"cx", "cy", "cz", "fx", "fy", "fz"},
                       [this, impGainType]() -> const sva::ImpedanceVecd & {
                         return config_.impGains.at(impGainType).spring().vec();
                       },
                       [this, impGainType](const Eigen::Vector6d & v) {
                         config_.impGains.at(impGainType).spring().vec(v);
                         requireImpGainUpdate_ = true;
                       }));
    gui.addElement({ctl().name(), config_.name, "ImpedanceGains", impGainType},
                   mc_rtc::gui::ArrayInput(
                       "Wrench", {"cx", "cy", "cz", "fx", "fy", "fz"},
                       [this, impGainType]() -> const sva::ImpedanceVecd & {
                         return config_.impGains.at(impGainType).wrench().vec();
                       },
                       [this, impGainType](const Eigen::Vector6d & v) {
                         config_.impGains.at(impGainType).wrench().vec(v);
                         requireImpGainUpdate_ = true;
                       }));
  }
}

void FootManager::removeFromGUI(mc_rtc::gui::StateBuilder & gui)
{
  gui.removeCategory({ctl().name(), config_.name});
}

void FootManager::addToLogger(mc_rtc::Logger & logger)
{
  logger.addLogEntry(config_.name + "_footstepQueueSize", this, [this]() { return footstepQueue_.size(); });

  for(const auto & foot : Feet::Both)
  {
    logger.addLogEntry(config_.name + "_targetFootPose_" + std::to_string(foot), this,
                       [this, foot]() -> const sva::PTransformd & { return targetFootPoses_.at(foot); });

    logger.addLogEntry(config_.name + "_targetFootVel_" + std::to_string(foot), this,
                       [this, foot]() -> const sva::MotionVecd & { return targetFootVels_.at(foot); });

    logger.addLogEntry(config_.name + "_targetFootAccel_" + std::to_string(foot), this,
                       [this, foot]() -> const sva::MotionVecd & { return targetFootAccels_.at(foot); });
  }

  logger.addLogEntry(config_.name + "_supportPhase", this, [this]() { return std::to_string(supportPhase_); });

  logger.addLogEntry(config_.name + "_refZmp", this, [this]() { return calcRefZmp(ctl().t()); });

  logger.addLogEntry(config_.name + "_refGroundPosZ", this, [this]() { return calcRefGroundPosZ(ctl().t()); });

  logger.addLogEntry(config_.name + "_leftFootSupportRatio", this, [this]() { return leftFootSupportRatio(); });

  logger.addLogEntry(config_.name + "_touchDown", this, [this]() { return touchDown_; });

  logger.addLogEntry(config_.name + "_touchDownRemainingDuration", this,
                     [this]() { return touchDownRemainingDuration(); });

  for(const auto & foot : Feet::Both)
  {
    logger.addLogEntry(config_.name + "_impGainType_" + std::to_string(foot), this,
                       [this, foot]() -> const std::string & { return impGainTypes_.at(foot); });
  }
}

void FootManager::removeFromLogger(mc_rtc::Logger & logger)
{
  logger.removeLogEntries(this);
}

const std::string & FootManager::surfaceName(const Foot & foot) const
{
  return ctl().footTasks_.at(foot)->surface();
}

Footstep FootManager::makeFootstep(const Foot & foot,
                                   const sva::PTransformd & footMidpose,
                                   double startTime,
                                   const mc_rtc::Configuration & mcRtcConfig) const
{
  Footstep footstep(foot, config_.midToFootTranss.at(foot) * footMidpose, startTime,
                    startTime + 0.5 * config_.doubleSupportRatio * config_.footstepDuration,
                    startTime + (1.0 - 0.5 * config_.doubleSupportRatio) * config_.footstepDuration,
                    startTime + config_.footstepDuration);
  footstep.config.load(mcRtcConfig);
  return footstep;
}

bool FootManager::appendFootstep(const Footstep & newFootstep)
{
  // Check time of new footstep
  if(newFootstep.transitStartTime < ctl().t())
  {
    mc_rtc::log::error("[FootManager] Ignore a new footstep with past time: {} < {}", newFootstep.transitStartTime,
                       ctl().t());
    return false;
  }
  if(!footstepQueue_.empty())
  {
    const Footstep & lastFootstep = footstepQueue_.back();
    if(newFootstep.transitStartTime < lastFootstep.transitEndTime)
    {
      mc_rtc::log::error("[FootManager] Ignore a new footstep earlier than the last footstep: {} < {}",
                         newFootstep.transitStartTime, lastFootstep.transitEndTime);
      return false;
    }
  }

  // Push to the queue
  footstepQueue_.push_back(newFootstep);

  return true;
}

Eigen::Vector3d FootManager::calcRefZmp(double t, int derivOrder) const
{
  if(derivOrder == 0)
  {
    return (*zmpFunc_)(t) + overwriteLandingPosLowPass_.eval();
  }
  else
  {
    return zmpFunc_->derivative(t, derivOrder);
  }
}

double FootManager::calcRefGroundPosZ(double t, int derivOrder) const
{
  if(derivOrder == 0)
  {
    return (*groundPosZFunc_)(t) + overwriteLandingPosLowPass_.eval().z();
  }
  else
  {
    return groundPosZFunc_->derivative(t, derivOrder);
  }
}

std::unordered_map<Foot, sva::PTransformd> FootManager::calcContactFootPoses(double t) const
{
  auto it = contactFootPosesList_.upper_bound(t);
  if(it == contactFootPosesList_.begin())
  {
    return std::unordered_map<Foot, sva::PTransformd>{};
  }
  else
  {
    it--;
    return it->second;
  }
}

std::set<Foot> FootManager::getCurrentContactFeet() const
{
  if(supportPhase_ == SupportPhase::DoubleSupport)
  {
    return Feet::Both;
  }
  else
  {
    if(config_.enableWrenchDistForTouchDownFoot && touchDown_)
    {
      return Feet::Both;
    }
    else
    {
      if(supportPhase_ == SupportPhase::LeftSupport)
      {
        return std::set<Foot>{Foot::Left};
      }
      else // if(supportPhase_ == SupportPhase::RightSupport)
      {
        return std::set<Foot>{Foot::Right};
      }
    }
  }
}

std::unordered_map<Foot, std::shared_ptr<Contact>> FootManager::calcCurrentContactList() const
{
  // Set contactList
  std::unordered_map<Foot, std::shared_ptr<Contact>> contactList;
  for(const auto & foot : getCurrentContactFeet())
  {
    std::vector<Eigen::Vector3d> localVertexList;
    const auto & surface = ctl().robot().surface(surfaceName(foot));
    for(const auto & point : surface.points())
    {
      // Surface points are represented in body frame, not surface frame
      localVertexList.push_back((point * surface.X_b_s().inv()).translation());
    }
    contactList.emplace(foot, std::make_shared<Contact>(std::to_string(foot), config_.fricCoeff, localVertexList,
                                                        targetFootPoses_.at(foot)));
  }

  return contactList;
}

double FootManager::leftFootSupportRatio() const
{
  if(footstepQueue_.empty())
  {
    return 0.5;
  }

  const Footstep & footstep = footstepQueue_.front();

  if(ctl().t() <= footstep.transitStartTime)
  {
    return 0.5;
  }
  else if(ctl().t() <= footstep.swingStartTime)
  {
    double ratio = (ctl().t() - footstep.transitStartTime) / (footstep.swingStartTime - footstep.transitStartTime);
    return -0.5 * sign(footstep.foot) * mc_filter::utils::clamp(ratio, 0.0, 1.0) + 0.5;
  }
  else if(ctl().t() <= footstep.swingEndTime)
  {
    return footstep.foot == Foot::Left ? 0 : 1;
  }
  else // if(ctl().t() <= footstep.transitEndTime)
  {
    double ratio = (footstep.transitEndTime - ctl().t()) / (footstep.transitEndTime - footstep.swingEndTime);
    return -0.5 * sign(footstep.foot) * mc_filter::utils::clamp(ratio, 0.0, 1.0) + 0.5;
  }
}

Eigen::Vector3d FootManager::calcZmpWithOffset(const Foot & foot, const sva::PTransformd & footPose) const
{
  Eigen::Vector3d zmpOffset = config_.zmpOffset;
  if(foot == Foot::Right)
  {
    zmpOffset.y() *= -1;
  }
  return (sva::PTransformd(zmpOffset) * footPose).translation();
}

Eigen::Vector3d FootManager::calcZmpWithOffset(const std::unordered_map<Foot, sva::PTransformd> & footPoses) const
{
  if(footPoses.size() == 0)
  {
    mc_rtc::log::error("[FootManager] footPoses is empty in calcZmpWithOffset.");
    return Eigen::Vector3d::Zero();
  }
  else if(footPoses.size() == 1)
  {
    return calcZmpWithOffset(footPoses.begin()->first, footPoses.begin()->second);
  }
  else // if(footPoses.size() == 2)
  {
    return 0.5
           * (calcZmpWithOffset(Foot::Left, footPoses.at(Foot::Left))
              + calcZmpWithOffset(Foot::Right, footPoses.at(Foot::Right)));
  }
}

void FootManager::updateFootTraj()
{
  // Disable hold mode by default
  for(const auto & foot : Feet::Both)
  {
    ctl().footTasks_.at(foot)->hold(false);
  }

  // Remove old footsteps from footstepQueue_
  while(!footstepQueue_.empty() && footstepQueue_.front().transitEndTime < ctl().t())
  {
    prevFootstep_ = std::make_shared<Footstep>(footstepQueue_.front());
    footstepQueue_.pop_front();
  }

  if(!footstepQueue_.empty() && footstepQueue_.front().swingStartTime <= ctl().t()
     && ctl().t() <= footstepQueue_.front().swingEndTime)
  {
    // Single support phase
    if(swingFootstep_)
    {
      // Check if swingFootstep_ is consistent
      if(swingFootstep_ != &(footstepQueue_.front()))
      {
        mc_rtc::log::error_and_throw("[FootManager] Swing footstep is not consistent.");
      }
    }
    else
    {
      // Set swingFootstep_
      swingFootstep_ = &(footstepQueue_.front());

      // Set swingPosFunc_ and swingRotFunc_
      {
        // Enable hold mode to prevent IK target pose from jumping
        // https://github.com/jrl-umi3218/mc_rtc/pull/143
        ctl().footTasks_.at(swingFootstep_->foot)->hold(true);

        const sva::PTransformd & swingStartPose = ctl().robot().surfacePose(surfaceName(swingFootstep_->foot));
        sva::PTransformd swingGoalPose = swingFootstep_->pose;
        if(config_.overwriteLandingPose && prevFootstep_)
        {
          sva::PTransformd swingRelPose = swingFootstep_->pose * prevFootstep_->pose.inv();
          swingGoalPose = swingRelPose * targetFootPoses_.at(prevFootstep_->foot);
        }
        double withdrawDuration = swingFootstep_->config.withdrawDurationRatio
                                  * (swingFootstep_->swingEndTime - swingFootstep_->swingStartTime);
        double approachDuration = swingFootstep_->config.approachDurationRatio
                                  * (swingFootstep_->swingEndTime - swingFootstep_->swingStartTime);

        BoundaryConstraint<Eigen::Vector3d> zeroVelBC(BoundaryConstraintType::Velocity, Eigen::Vector3d::Zero());
        BoundaryConstraint<Eigen::Vector3d> zeroAccelBC(BoundaryConstraintType::Acceleration, Eigen::Vector3d::Zero());

        // Spline to withdraw foot
        // Pos
        std::map<double, Eigen::Vector3d> withdrawPosWaypoints = {
            {swingFootstep_->swingStartTime, swingStartPose.translation()},
            {swingFootstep_->swingStartTime + withdrawDuration,
             (sva::PTransformd(swingFootstep_->config.withdrawOffset) * swingStartPose).translation()}};
        auto withdrawPosSpline =
            std::make_shared<CubicSpline<Eigen::Vector3d>>(3, withdrawPosWaypoints, zeroVelBC, zeroAccelBC);
        withdrawPosSpline->calcCoeff();
        swingPosFunc_->appendFunc(swingFootstep_->swingStartTime + withdrawDuration, withdrawPosSpline);
        // Rot
        swingRotFunc_->appendPoint(
            std::make_pair(swingFootstep_->swingStartTime, swingStartPose.rotation().transpose()));
        swingRotFunc_->appendPoint(
            std::make_pair(swingFootstep_->swingStartTime + withdrawDuration, swingStartPose.rotation().transpose()));

        // Spline to approach foot
        // Pos
        std::map<double, Eigen::Vector3d> approachPosWaypoints = {
            {swingFootstep_->swingEndTime - approachDuration,
             (sva::PTransformd(swingFootstep_->config.approachOffset) * swingGoalPose).translation()},
            {swingFootstep_->swingEndTime, swingGoalPose.translation()}};
        auto approachPosSpline =
            std::make_shared<CubicSpline<Eigen::Vector3d>>(3, approachPosWaypoints, zeroAccelBC, zeroVelBC);
        approachPosSpline->calcCoeff();
        swingPosFunc_->appendFunc(swingFootstep_->swingEndTime, approachPosSpline);
        // Rot
        swingRotFunc_->appendPoint(std::make_pair(swingFootstep_->swingEndTime - approachDuration,
                                                  swingFootstep_->pose.rotation().transpose()));
        swingRotFunc_->appendPoint(
            std::make_pair(swingFootstep_->swingEndTime, swingFootstep_->pose.rotation().transpose()));

        // Spline to swing foot
        // Pos
        std::map<double, Eigen::Vector3d> swingPosWaypoints = {
            *withdrawPosWaypoints.rbegin(),
            {0.5 * (swingFootstep_->swingStartTime + swingFootstep_->swingEndTime),
             (sva::PTransformd(swingFootstep_->config.swingOffset)
              * sva::interpolate(swingStartPose, swingGoalPose, 0.5))
                 .translation()},
            *approachPosWaypoints.begin()};
        auto swingPosSpline = std::make_shared<CubicSpline<Eigen::Vector3d>>(
            3, swingPosWaypoints,
            BoundaryConstraint<Eigen::Vector3d>(
                BoundaryConstraintType::Velocity,
                withdrawPosSpline->derivative(swingFootstep_->swingStartTime + withdrawDuration, 1)),
            BoundaryConstraint<Eigen::Vector3d>(
                BoundaryConstraintType::Velocity,
                approachPosSpline->derivative(swingFootstep_->swingEndTime - approachDuration, 1)));
        swingPosSpline->calcCoeff();
        swingPosFunc_->appendFunc(swingFootstep_->swingEndTime - approachDuration, swingPosSpline);
        // Rot
        swingRotFunc_->calcCoeff();
      }

      // Set baseYawFunc_
      {
        double swingStartBaseYaw =
            mc_rbdyn::rpyFromMat(interpolate<Eigen::Matrix3d>(targetFootPoses_.at(Foot::Left).rotation().transpose(),
                                                              targetFootPoses_.at(Foot::Right).rotation().transpose(),
                                                              0.5)
                                     .transpose())
                .z();
        baseYawFunc_->appendPoint(
            std::make_pair(swingFootstep_->swingStartTime,
                           Eigen::AngleAxisd(swingStartBaseYaw, Eigen::Vector3d::UnitZ()).toRotationMatrix()));

        double swingEndBaseYaw =
            mc_rbdyn::rpyFromMat(interpolate<Eigen::Matrix3d>(
                                     swingFootstep_->pose.rotation().transpose(),
                                     targetFootPoses_.at(opposite(swingFootstep_->foot)).rotation().transpose(), 0.5)
                                     .transpose())
                .z();
        baseYawFunc_->appendPoint(
            std::make_pair(swingFootstep_->swingEndTime,
                           Eigen::AngleAxisd(swingEndBaseYaw, Eigen::Vector3d::UnitZ()).toRotationMatrix()));

        baseYawFunc_->calcCoeff();
      }

      // Set supportPhase_
      if(swingFootstep_->foot == Foot::Left)
      {
        supportPhase_ = SupportPhase::RightSupport;
      }
      else // if(swingFootstep_->foot == Foot::Right)
      {
        supportPhase_ = SupportPhase::LeftSupport;
      }
    }

    // Update target
    if(!(config_.stopSwingTrajForTouchDownFoot && touchDown_))
    {
      targetFootPoses_.at(swingFootstep_->foot) =
          sva::PTransformd((*swingRotFunc_)(ctl().t()).transpose(), (*swingPosFunc_)(ctl().t()));
      targetFootVels_.at(swingFootstep_->foot) =
          sva::MotionVecd(swingRotFunc_->derivative(ctl().t(), 1), swingPosFunc_->derivative(ctl().t(), 1));
      targetFootAccels_.at(swingFootstep_->foot) =
          sva::MotionVecd(swingRotFunc_->derivative(ctl().t(), 2), swingPosFunc_->derivative(ctl().t(), 2));
    }

    // Update touchDown_
    if(!touchDown_ && detectTouchDown())
    {
      touchDown_ = true;

      if(config_.stopSwingTrajForTouchDownFoot)
      {
        targetFootVels_.at(swingFootstep_->foot) = sva::MotionVecd::Zero();
        targetFootAccels_.at(swingFootstep_->foot) = sva::MotionVecd::Zero();
      }
    }
  }
  else
  {
    // Double support phase
    if(swingFootstep_)
    {
      // Update target
      if(!(config_.keepSupportFootPoseForTouchDownFoot && touchDown_))
      {
        targetFootPoses_.at(swingFootstep_->foot) = sva::PTransformd(
            (*swingRotFunc_)(swingFootstep_->swingEndTime).transpose(), (*swingPosFunc_)(swingFootstep_->swingEndTime));
        targetFootVels_.at(swingFootstep_->foot) = sva::MotionVecd::Zero();
        targetFootAccels_.at(swingFootstep_->foot) = sva::MotionVecd::Zero();
      }

      lastDoubleSupportFootPoses_.at(swingFootstep_->foot) = swingFootstep_->pose;

      // Set supportPhase_
      supportPhase_ = SupportPhase::DoubleSupport;

      // Clear swingPosFunc_ and swingRotFunc_
      swingPosFunc_->clearFuncs();
      swingRotFunc_->clearPoints();

      // Clear baseYawFunc_
      baseYawFunc_->clearPoints();

      // Clear touchDown_
      touchDown_ = false;

      // Clear swingFootstep_
      swingFootstep_ = nullptr;
    }
  }

  // Set target of foot tasks
  for(const auto & foot : Feet::Both)
  {
    ctl().footTasks_.at(foot)->targetPose(targetFootPoses_.at(foot));
    // ImpedanceTask::targetVel receive the velocity represented in the world frame
    ctl().footTasks_.at(foot)->targetVel(targetFootVels_.at(foot));
    // ImpedanceTask::targetAccel receive the acceleration represented in the world frame
    ctl().footTasks_.at(foot)->targetAccel(targetFootAccels_.at(foot));
  }

  // Update impGainTypes_ and requireImpGainUpdate_
  std::unordered_map<Foot, std::string> newImpGainTypes;
  const auto & contactFeet = getCurrentContactFeet();
  if(contactFeet.size() == 1)
  {
    newImpGainTypes.emplace(*(contactFeet.cbegin()), "singleSupport");
    newImpGainTypes.emplace(opposite(*(contactFeet.cbegin())), "swing");
  }
  else // if(contactFeet.size() == 2)
  {
    for(const auto & foot : Feet::Both)
    {
      newImpGainTypes.emplace(foot, "doubleSupport");
    }
  }
  for(const auto & foot : Feet::Both)
  {
    if(requireImpGainUpdate_)
    {
      break;
    }
    if(impGainTypes_.at(foot) != newImpGainTypes.at(foot))
    {
      requireImpGainUpdate_ = true;
    }
  }
  impGainTypes_ = newImpGainTypes;

  // Set impedance gains of foot tasks
  if(requireImpGainUpdate_)
  {
    requireImpGainUpdate_ = false;

    for(const auto & foot : Feet::Both)
    {
      ctl().footTasks_.at(foot)->gains() = config_.impGains.at(impGainTypes_.at(foot));
    }
  }

  // Set target of base link orientation task
  if(supportPhase_ == SupportPhase::DoubleSupport)
  {
    const sva::PTransformd & footMidpose =
        sva::interpolate(targetFootPoses_.at(Foot::Left), targetFootPoses_.at(Foot::Right), 0.5);
    ctl().baseOriTask_->orientation(sva::RotZ(mc_rbdyn::rpyFromMat(footMidpose.rotation()).z()));
    ctl().baseOriTask_->refVel(Eigen::Vector3d::Zero());
    ctl().baseOriTask_->refAccel(Eigen::Vector3d::Zero());
  }
  else
  {
    ctl().baseOriTask_->orientation((*baseYawFunc_)(ctl().t()).transpose());
    ctl().baseOriTask_->refVel(baseYawFunc_->derivative(ctl().t(), 1));
    ctl().baseOriTask_->refAccel(baseYawFunc_->derivative(ctl().t(), 2));
  }

  // Update footstep visualization
  std::vector<std::vector<Eigen::Vector3d>> footstepPolygonList;
  for(const auto & footstep : footstepQueue_)
  {
    std::vector<Eigen::Vector3d> footstepPolygon;
    const auto & surface = ctl().robot().surface(surfaceName(footstep.foot));
    for(const auto & point : surface.points())
    {
      // Surface points are represented in body frame, not surface frame
      footstepPolygon.push_back((point * surface.X_b_s().inv() * footstep.pose).translation());
    }
    footstepPolygonList.push_back(footstepPolygon);
  }

  ctl().gui()->removeCategory({ctl().name(), config_.name, "FootstepMarker"});
  ctl().gui()->addElement({ctl().name(), config_.name, "FootstepMarker"},
                          mc_rtc::gui::Polygon("Footstep", {mc_rtc::gui::Color::Blue, 0.02},
                                               [footstepPolygonList]() { return footstepPolygonList; }));
}

void FootManager::updateZmpTraj()
{
  zmpFunc_->clearPoints();
  groundPosZFunc_->clearPoints();
  contactFootPosesList_.clear();

  std::unordered_map<Foot, sva::PTransformd> footPoses = lastDoubleSupportFootPoses_;

  auto calcFootMidposZ = [](const std::unordered_map<Foot, sva::PTransformd> & _footPoses) {
    return 0.5 * (_footPoses.at(Foot::Left).translation().z() + _footPoses.at(Foot::Right).translation().z());
  };

  if(footstepQueue_.empty() || ctl().t() < footstepQueue_.front().transitStartTime)
  {
    // Set initial point
    zmpFunc_->appendPoint(std::make_pair(ctl().t(), calcZmpWithOffset(footPoses)));
    groundPosZFunc_->appendPoint(std::make_pair(ctl().t(), calcFootMidposZ(footPoses)));
    contactFootPosesList_.emplace(ctl().t(), footPoses);
  }

  for(const auto & footstep : footstepQueue_)
  {
    Foot supportFoot = opposite(footstep.foot);
    Eigen::Vector3d supportFootZmp = calcZmpWithOffset(supportFoot, footPoses.at(supportFoot));

    zmpFunc_->appendPoint(std::make_pair(footstep.transitStartTime, calcZmpWithOffset(footPoses)));
    groundPosZFunc_->appendPoint(std::make_pair(footstep.transitStartTime, calcFootMidposZ(footPoses)));
    contactFootPosesList_.emplace(footstep.transitStartTime, footPoses);

    zmpFunc_->appendPoint(std::make_pair(footstep.swingStartTime, supportFootZmp));
    groundPosZFunc_->appendPoint(std::make_pair(footstep.swingStartTime, calcFootMidposZ(footPoses)));
    contactFootPosesList_.emplace(footstep.swingStartTime,
                                  std::unordered_map<Foot, sva::PTransformd>{{supportFoot, footPoses.at(supportFoot)}});

    // Update footPoses
    footPoses.at(footstep.foot) = footstep.pose;

    zmpFunc_->appendPoint(std::make_pair(footstep.swingEndTime, supportFootZmp));
    groundPosZFunc_->appendPoint(std::make_pair(footstep.swingEndTime, calcFootMidposZ(footPoses)));
    contactFootPosesList_.emplace(footstep.swingEndTime, footPoses);

    groundPosZFunc_->appendPoint(std::make_pair(footstep.transitEndTime, calcFootMidposZ(footPoses)));
    zmpFunc_->appendPoint(std::make_pair(footstep.transitEndTime, calcZmpWithOffset(footPoses)));
    contactFootPosesList_.emplace(footstep.transitEndTime, footPoses);

    if(ctl().t() + config_.zmpHorizon <= footstep.transitEndTime)
    {
      break;
    }
  }

  if(footstepQueue_.empty() || footstepQueue_.back().transitEndTime < ctl().t() + config_.zmpHorizon)
  {
    // Set terminal point
    zmpFunc_->appendPoint(std::make_pair(ctl().t() + config_.zmpHorizon, calcZmpWithOffset(footPoses)));
    groundPosZFunc_->appendPoint(std::make_pair(ctl().t() + config_.zmpHorizon, calcFootMidposZ(footPoses)));
  }

  zmpFunc_->calcCoeff();
  groundPosZFunc_->calcCoeff();

  // Update low-pass filter for the overwrite amount of landing position
  if(config_.overwriteLandingPose)
  {
    Eigen::Vector3d overwriteLandingMeanPos = Eigen::Vector3d::Zero();
    const auto & currentContactFootPoses = calcContactFootPoses(ctl().t());
    if(currentContactFootPoses.size() == 0)
    {
      overwriteLandingMeanPos.setZero();
    }
    else
    {
      for(const auto & contactFootPosesKV : currentContactFootPoses)
      {
        const Foot & foot = contactFootPosesKV.first;
        const sva::PTransformd & originalLandingPose = contactFootPosesKV.second;
        sva::PTransformd overwriteLandingPose = targetFootPoses_.at(foot);
        overwriteLandingMeanPos += overwriteLandingPose.translation() - originalLandingPose.translation();
      }
      overwriteLandingMeanPos /= currentContactFootPoses.size();
    }
    overwriteLandingPosLowPass_.update(overwriteLandingMeanPos);
  }
  else
  {
    overwriteLandingPosLowPass_.update(Eigen::Vector3d::Zero());
  }
}

double FootManager::touchDownRemainingDuration() const
{
  if(supportPhase_ == SupportPhase::DoubleSupport)
  {
    return 0;
  }
  else
  {
    return swingFootstep_->swingEndTime - ctl().t();
  }
}

bool FootManager::detectTouchDown() const
{
  // False for double support phase
  if(supportPhase_ == SupportPhase::DoubleSupport)
  {
    return false;
  }

  // False if the remaining duration does not meet the threshold
  if(touchDownRemainingDuration() > config_.touchDownRemainingDuration)
  {
    return false;
  }

  // False if the position error does not meet the threshold
  Foot swingFoot = (supportPhase_ == SupportPhase::LeftSupport ? Foot::Right : Foot::Left);
  if(((*swingPosFunc_)(swingFootstep_->swingEndTime) - (*swingPosFunc_)(ctl().t())).norm() > config_.touchDownPosError)
  {
    return false;
  }

  // False if the normal force does not meet the threshold
  double fz = ctl().robot().surfaceWrench(surfaceName(swingFoot)).force().z();
  if(fz < config_.touchDownForceZ)
  {
    return false;
  }

  return true;
}
