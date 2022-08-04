#include <mc_rtc/gui/Checkbox.h>
#include <mc_rtc/gui/Label.h>
#include <mc_rtc/gui/NumberInput.h>
#include <mc_tasks/CoMTask.h>
#include <mc_tasks/OrientationTask.h>

#include <CCC/Constants.h>

#include <BaselineWalkingController/BaselineWalkingController.h>
#include <BaselineWalkingController/CentroidalManager.h>
#include <BaselineWalkingController/FootManager.h>
#include <BaselineWalkingController/tasks/FirstOrderImpedanceTask.h>
#include <BaselineWalkingController/wrench/Contact.h>
#include <BaselineWalkingController/wrench/WrenchDistribution.h>

using namespace BWC;

void CentroidalManager::Configuration::load(const mc_rtc::Configuration & mcRtcConfig)
{
  mcRtcConfig("name", name);
  mcRtcConfig("method", method);
  mcRtcConfig("useActualStateForMpc", useActualStateForMpc);
  mcRtcConfig("enableDcmFeedback", enableDcmFeedback);
  mcRtcConfig("enableComZFeedback", enableComZFeedback);
  mcRtcConfig("dcmGain", dcmGain);
  mcRtcConfig("zmpVelGain", zmpVelGain);
  mcRtcConfig("comZGainP", comZGainP);
  mcRtcConfig("comZGainD", comZGainD);
  mcRtcConfig("refComZ", refComZ);
  mcRtcConfig("useTargetPoseForControlRobotAnchorFrame", useTargetPoseForControlRobotAnchorFrame);
  mcRtcConfig("useActualCoMForWrenchDistribution", useActualCoMForWrenchDistribution);
  mcRtcConfig("wrenchDistConfig", wrenchDistConfig);
}

CentroidalManager::CentroidalManager(BaselineWalkingController * ctlPtr, const mc_rtc::Configuration & mcRtcConfig)
: ctlPtr_(ctlPtr)
{
}

void CentroidalManager::reset()
{
  robotMass_ = ctl().robot().mass();
}

void CentroidalManager::update()
{
  // Set MPC state
  if(config().useActualStateForMpc)
  {
    mpcCom_ = ctl().realRobot().com();
    mpcComVel_ = ctl().realRobot().comVelocity();
  }
  else
  {
    // Task targets are the planned state in the previous step
    mpcCom_ = ctl().comTask_->com();
    mpcComVel_ = ctl().comTask_->refVel();
  }

  // Run MPC
  runMpc();

  // Calculate command wrench
  {
    // Compensate ZMP delay
    // See equation (10) of https://ieeexplore.ieee.org/abstract/document/6094838
    Eigen::Vector3d refZmpVel = ctl().footManager_->calcRefZmp(ctl().t(), 1);
    controlZmp_ << plannedZmp_.head<2>() + config().zmpVelGain * refZmpVel.head<2>(), plannedZmp_.z();

    // Apply DCM feedback
    double omega = std::sqrt(plannedForceZ_ / (robotMass_ * mpcCom_.z()));
    Eigen::Vector3d plannedDcm = ctl().comTask_->com() + ctl().comTask_->refVel() / omega;
    Eigen::Vector3d actualDcm = ctl().realRobot().com() + ctl().realRobot().comVelocity() / omega;
    if(config().enableDcmFeedback)
    {
      controlZmp_.head<2>() += config().dcmGain * (actualDcm - plannedDcm).head<2>();
    }

    // Apply ForceZ feedback
    controlForceZ_ = plannedForceZ_;
    if(config().enableComZFeedback)
    {
      double plannedComZ = ctl().comTask_->com().z();
      double actualComZ = ctl().realRobot().com().z();
      double plannedComVelZ = ctl().comTask_->refVel().z();
      double actualComVelZ = ctl().realRobot().comVelocity().z();
      controlForceZ_ -=
          config().comZGainP * (actualComZ - plannedComZ) + config().comZGainD * (actualComVelZ - plannedComVelZ);
    }

    // Project ZMP
    // TODO
    controlZmp_;

    // Convert ZMP to wrench and distribute
    contactList_ = ctl().footManager_->calcContactList();
    if(!wrenchDist_ || wrenchDist_->contactList_ != contactList_)
    {
      wrenchDist_ = std::make_shared<WrenchDistribution>(contactList_, config().wrenchDistConfig);
    }
    Eigen::Vector3d comForWrenchDist =
        (config().useActualCoMForWrenchDistribution ? ctl().realRobot().com() : ctl().comTask_->com());
    sva::ForceVecd controlWrench;
    controlWrench.force() << controlForceZ_ / comForWrenchDist.z()
                                 * (comForWrenchDist.head<2>() - controlZmp_.head<2>()),
        controlForceZ_;
    controlWrench.moment().setZero();
    wrenchDist_->run(controlWrench, comForWrenchDist);
  }

  // Set target of tasks
  {
    // Set target of CoM task
    Eigen::Vector3d plannedComAccel;
    plannedComAccel << plannedForceZ_ / (robotMass_ * mpcCom_.z()) * (mpcCom_.head<2>() - plannedZmp_.head<2>()),
        plannedForceZ_ / robotMass_;
    plannedComAccel.z() -= CCC::constants::g;
    Eigen::Vector3d nextPlannedCom =
        mpcCom_ + ctl().dt() * mpcComVel_ + 0.5 * std::pow(ctl().dt(), 2) * plannedComAccel;
    Eigen::Vector3d nextPlannedComVel = mpcComVel_ + ctl().dt() * plannedComAccel;
    if(isConstantComZ())
    {
      nextPlannedCom.z() = config().refComZ;
      nextPlannedComVel.z() = 0;
      plannedComAccel.z() = 0;
    }
    ctl().comTask_->com(nextPlannedCom);
    ctl().comTask_->refVel(nextPlannedComVel);
    ctl().comTask_->refAccel(plannedComAccel);

    // Set target of base link orientation task
    const sva::PTransformd & footMidpose = sva::interpolate(ctl().footManager_->targetFootPose(Foot::Left),
                                                            ctl().footManager_->targetFootPose(Foot::Right), 0.5);
    ctl().baseOriTask_->orientation(sva::RotZ(mc_rbdyn::rpyFromMat(footMidpose.rotation()).z()));
    ctl().baseOriTask_->refVel(Eigen::Vector3d::Zero());
    ctl().baseOriTask_->refAccel(Eigen::Vector3d::Zero());

    // Set target wrench of foot tasks
    const auto & targetWrenchList = wrenchDist_->calcWrenchList();
    for(const auto & foot : Feet::Both)
    {
      sva::ForceVecd targetWrench = sva::ForceVecd::Zero();
      if(targetWrenchList.count(foot))
      {
        targetWrench = targetWrenchList.at(foot);
      }
      ctl().footTasks_.at(foot)->targetWrenchW(targetWrench);
    }
  }
}

void CentroidalManager::addToGUI(mc_rtc::gui::StateBuilder & gui)
{
  gui.addElement(
      {"BWC", config().name}, mc_rtc::gui::Label("method", [this]() { return config().method; }),
      mc_rtc::gui::Checkbox(
          "useActualStateForMpc", [this]() { return config().useActualStateForMpc; },
          [this]() { config().useActualStateForMpc = !config().useActualStateForMpc; }),
      mc_rtc::gui::Checkbox(
          "enableDcmFeedback", [this]() { return config().enableDcmFeedback; },
          [this]() { config().enableDcmFeedback = !config().enableDcmFeedback; }),
      mc_rtc::gui::Checkbox(
          "enableComZFeedback", [this]() { return config().enableComZFeedback; },
          [this]() { config().enableComZFeedback = !config().enableComZFeedback; }),
      mc_rtc::gui::NumberInput(
          "dcmGain", [this]() { return config().dcmGain; }, [this](double v) { config().dcmGain = v; }),
      mc_rtc::gui::NumberInput(
          "zmpVelGain", [this]() { return config().zmpVelGain; }, [this](double v) { config().zmpVelGain = v; }),
      mc_rtc::gui::NumberInput(
          "comZGainP", [this]() { return config().comZGainP; }, [this](double v) { config().comZGainP = v; }),
      mc_rtc::gui::NumberInput(
          "comZGainD", [this]() { return config().comZGainD; }, [this](double v) { config().comZGainD = v; }),
      mc_rtc::gui::NumberInput(
          "refComZ", [this]() { return config().refComZ; }, [this](double v) { config().refComZ = v; }),
      mc_rtc::gui::Checkbox(
          "useTargetPoseForControlRobotAnchorFrame",
          [this]() { return config().useTargetPoseForControlRobotAnchorFrame; },
          [this]() {
            config().useTargetPoseForControlRobotAnchorFrame = !config().useTargetPoseForControlRobotAnchorFrame;
          }),
      mc_rtc::gui::Checkbox(
          "useActualCoMForWrenchDistribution", [this]() { return config().useActualCoMForWrenchDistribution; },
          [this]() { config().useActualCoMForWrenchDistribution = !config().useActualCoMForWrenchDistribution; }));
}

void CentroidalManager::removeFromGUI(mc_rtc::gui::StateBuilder & gui)
{
  gui.removeCategory({"BWC", config().name});
}

void CentroidalManager::addToLogger(mc_rtc::Logger & logger)
{
  MC_RTC_LOG_HELPER(config().name + "_CoM_MPC", mpcCom_);
  logger.addLogEntry(config().name + "_CoM_target", this, [this]() { return ctl().comTask_->com(); });
  logger.addLogEntry(config().name + "_CoM_control", this, [this]() { return ctl().robot().com(); });
  logger.addLogEntry(config().name + "_CoM_real", this, [this]() { return ctl().realRobot().com(); });

  logger.addLogEntry(config().name + "_ZMP_ref", this, [this]() { return ctl().footManager_->calcRefZmp(ctl().t()); });
  MC_RTC_LOG_HELPER(config().name + "_ZMP_planned", plannedZmp_);
  MC_RTC_LOG_HELPER(config().name + "_ZMP_control", controlZmp_);
  logger.addLogEntry(config().name + "_ZMP_controlWrenchDist", this, [this]() {
    return wrenchDist_ ? calcZmp(wrenchDist_->calcWrenchList()) : Eigen::Vector3d::Zero();
  });
  logger.addLogEntry(config().name + "_ZMP_measured", this, [this]() {
    std::unordered_map<Foot, sva::ForceVecd> sensorWrenchList;
    for(const auto & foot : ctl().footManager_->getContactFeet())
    {
      const auto & surfaceName = ctl().footManager_->surfaceName(foot);
      const auto & sensorName = ctl().robot().indirectSurfaceForceSensor(surfaceName).name();
      const auto & sensor = ctl().robot().forceSensor(sensorName);
      const auto & sensorWrench = sensor.worldWrenchWithoutGravity(ctl().robot());
      sensorWrenchList.emplace(foot, sensorWrench);
    }
    return calcZmp(sensorWrenchList);
  });
  logger.addLogEntry(config().name + "_ZMP_SupportRegion_min", this, [this]() {
    Eigen::Vector2d minPos = Eigen::Vector2d::Constant(std::numeric_limits<double>::max());
    for(const auto & contactKV : contactList_)
    {
      for(const auto & vertexWithRidge : contactKV.second->vertexWithRidgeList_)
      {
        minPos = minPos.cwiseMin(vertexWithRidge.vertex.head<2>());
      }
    }
    return minPos;
  });
  logger.addLogEntry(config().name + "_ZMP_SupportRegion_max", this, [this]() {
    Eigen::Vector2d maxPos = Eigen::Vector2d::Constant(std::numeric_limits<double>::lowest());
    for(const auto & contactKV : contactList_)
    {
      for(const auto & vertexWithRidge : contactKV.second->vertexWithRidgeList_)
      {
        maxPos = maxPos.cwiseMax(vertexWithRidge.vertex.head<2>());
      }
    }
    return maxPos;
  });

  MC_RTC_LOG_HELPER(config().name + "_forceZ_planned", plannedForceZ_);
  MC_RTC_LOG_HELPER(config().name + "_forceZ_control", controlForceZ_);
}

void CentroidalManager::removeFromLogger(mc_rtc::Logger & logger)
{
  logger.removeLogEntries(this);
}

void CentroidalManager::setAnchorFrame()
{
  std::string anchorName = "KinematicAnchorFrame::" + ctl().robot().name();
  if(ctl().datastore().has(anchorName))
  {
    ctl().datastore().remove(anchorName);
  }
  ctl().datastore().make_call(anchorName, [this](const mc_rbdyn::Robot & robot) { return calcAnchorFrame(robot); });
}

sva::PTransformd CentroidalManager::calcAnchorFrame(const mc_rbdyn::Robot & robot) const
{
  double leftFootSupportRatio = ctl().footManager_->leftFootSupportRatio();
  bool isControlRobot = (&(ctl().robot()) == &robot);

  if(isControlRobot && config().useTargetPoseForControlRobotAnchorFrame)
  {
    return sva::interpolate(ctl().footManager_->targetFootPose(Foot::Right),
                            ctl().footManager_->targetFootPose(Foot::Left), leftFootSupportRatio);
  }
  else
  {
    return sva::interpolate(robot.surfacePose(ctl().footManager_->surfaceName(Foot::Right)),
                            robot.surfacePose(ctl().footManager_->surfaceName(Foot::Left)), leftFootSupportRatio);
  }
}

Eigen::Vector3d CentroidalManager::calcZmp(const std::unordered_map<Foot, sva::ForceVecd> & wrenchList,
                                           double zmpPlaneHeight,
                                           const Eigen::Vector3d & zmpPlaneNormal) const
{
  sva::ForceVecd totalWrench = sva::ForceVecd::Zero();
  for(const auto & wrenchKV : wrenchList)
  {
    totalWrench += wrenchKV.second;
  }

  Eigen::Vector3d zmpPlaneOrigin = Eigen::Vector3d(0, 0, zmpPlaneHeight);
  Eigen::Vector3d zmp = zmpPlaneOrigin;

  if(totalWrench.force().z() > 0)
  {
    Eigen::Vector3d momentInZMPPlane = totalWrench.moment() - zmpPlaneOrigin.cross(totalWrench.force());
    zmp += zmpPlaneNormal.cross(momentInZMPPlane) / totalWrench.force().z();
  }

  return zmp;
}
