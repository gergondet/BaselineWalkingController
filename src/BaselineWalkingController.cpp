#include <sys/syscall.h>

#include <mc_tasks/CoMTask.h>
#include <mc_tasks/MetaTaskLoader.h>
#include <mc_tasks/OrientationTask.h>

#include <BaselineWalkingController/BaselineWalkingController.h>
#include <BaselineWalkingController/CentroidalManager.h>
#include <BaselineWalkingController/FootManager.h>
#include <BaselineWalkingController/centroidal/CentroidalManagerDdpZmp.h>
#include <BaselineWalkingController/centroidal/CentroidalManagerPreviewControlZmp.h>
#include <BaselineWalkingController/tasks/FirstOrderImpedanceTask.h>

using namespace BWC;

BaselineWalkingController::BaselineWalkingController(mc_rbdyn::RobotModulePtr rm,
                                                     double dt,
                                                     const mc_rtc::Configuration & _config)
: mc_control::fsm::Controller(rm, dt, _config)
{
  // Setup tasks
  if(!(config().has("CoMTask") && config().has("BaseOrientationTask") && config().has("FootTaskList")))
  {
    mc_rtc::log::error_and_throw("[BaselineWalkingController] Task configuration is missing.");
  }
  comTask_ = mc_tasks::MetaTaskLoader::load<mc_tasks::CoMTask>(solver(), config()("CoMTask"));
  comTask_->name("CoMTask");
  baseOriTask_ = mc_tasks::MetaTaskLoader::load<mc_tasks::OrientationTask>(solver(), config()("BaseOrientationTask"));
  baseOriTask_->name("BaseOriTask");
  for(const auto & footTaskConfig : config()("FootTaskList"))
  {
    Foot foot = strToFoot(footTaskConfig("foot"));
    footTasks_.emplace(foot, mc_tasks::MetaTaskLoader::load<FirstOrderImpedanceTask>(solver(), footTaskConfig));
    footTasks_.at(foot)->name("FootTask_" + std::to_string(foot));
  }

  // Setup managers
  if(!(config().has("FootManager") && config().has("CentroidalManager")))
  {
    mc_rtc::log::error_and_throw("[BaselineWalkingController] Manager configuration is missing.");
  }
  footManager_ = std::make_shared<FootManager>(this, config()("FootManager"));
  std::string centroidalManagerMethod = config()("CentroidalManager")("method", std::string(""));
  if(centroidalManagerMethod == "PreviewControlZmp")
  {
    centroidalManager_ = std::make_shared<CentroidalManagerPreviewControlZmp>(this, config()("CentroidalManager"));
  }
  else if(centroidalManagerMethod == "DdpZmp")
  {
    centroidalManager_ = std::make_shared<CentroidalManagerDdpZmp>(this, config()("CentroidalManager"));
  }
  else
  {
    mc_rtc::log::error_and_throw("[BaselineWalkingController] Invalid centroidalManagerMethod: {}.",
                                 centroidalManagerMethod);
  }

  // Setup anchor
  setDefaultAnchor();

  mc_rtc::log::success("[BaselineWalkingController] Constructed.");
}

void BaselineWalkingController::reset(const mc_control::ControllerResetData & resetData)
{
  mc_control::fsm::Controller::reset(resetData);

  enableManagerUpdate_ = false;

  // Print message to set priority
  long tid = static_cast<long>(syscall(SYS_gettid));
  mc_rtc::log::success("[BaselineWalkingController] TID is {}. Run the following command to set high priority:\n  sudo "
                       "renice -n -20 -p {}",
                       tid, tid);
  mc_rtc::log::success(
      "[BaselineWalkingController] You can check the current priority by the following command:\n  ps -p "
      "`pgrep choreonoid` -o pid,tid,args,ni,pri,wchan m");

  mc_rtc::log::success("[BaselineWalkingController] Reset.");
}

bool BaselineWalkingController::run()
{
  t_ += dt();

  if(enableManagerUpdate_)
  {
    // Update managers
    footManager_->update();
    centroidalManager_->update();
  }

  return mc_control::fsm::Controller::run();
}

void BaselineWalkingController::stop()
{
  // Clean up tasks
  solver().removeTask(comTask_);
  solver().removeTask(baseOriTask_);
  for(const auto & foot : Feet::Both)
  {
    solver().removeTask(footTasks_.at(foot));
  }

  // Clean up managers
  footManager_->removeFromGUI(*gui());
  footManager_->removeFromLogger(logger());
  footManager_.reset();
  centroidalManager_->removeFromGUI(*gui());
  centroidalManager_->removeFromLogger(logger());
  centroidalManager_.reset();

  // Clean up anchor
  setDefaultAnchor();

  mc_control::fsm::Controller::stop();
}

void BaselineWalkingController::setDefaultAnchor()
{
  std::string anchorName = "KinematicAnchorFrame::" + robot().name();
  if(datastore().has(anchorName))
  {
    datastore().remove(anchorName);
  }
  datastore().make_call(anchorName, [this](const mc_rbdyn::Robot & robot) {
    return sva::interpolate(robot.surfacePose(footManager_->surfaceName(Foot::Left)),
                            robot.surfacePose(footManager_->surfaceName(Foot::Right)), 0.5);
  });
}