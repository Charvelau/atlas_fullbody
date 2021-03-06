#include <ros/ros.h>
#include <signal.h>
#include <atlas_msgs/AtlasCommand.h>
#include <atlas_msgs/AtlasState.h>
#include <boost/thread.hpp>
#include <atlas_msgs/AtlasSimInterfaceCommand.h>

#include <cmu_walk/LipmWalkingCon.h>
#include <test_walk/cmu_ctrl_utils.h>

#include <cmu_walk/DummyFSPlanner.h>

static void quit(int sig);

static ros::Publisher pub_atlas_cmd;
static ros::Subscriber sub_atlas_state;

static boost::mutex state_lock;
static atlas_msgs::AtlasState data_from_robot;
static atlas_msgs::AtlasCommand data_to_robot;

static BatchLogger logger;           // logging

static void control_thread()
{

}


static void AtlasStateCallback(const atlas_msgs::AtlasState::ConstPtr &msg)
{
  boost::mutex::scoped_lock lock(state_lock);
  data_from_robot = *msg;
}  

int main(int argc, char **argv)
{
  signal( SIGINT, quit );
  ros::init(argc, argv, "test_walk", ros::init_options::NoSigintHandler);
  
  ros::NodeHandle nh;  
  ros::Time last_ros_time_;
  bool wait = true;
  while (wait) {
    last_ros_time_ = ros::Time::now();
    if (last_ros_time_.toSec() > 0) {
      wait = false;
    }
  }
  
  //////////////////////////////////////////////////////////////
  // ros pub / sub
  pub_atlas_cmd = nh.advertise<atlas_msgs::AtlasCommand>("/atlas/atlas_command", 10); 
  ros::SubscribeOptions jointStatesSo = ros::SubscribeOptions::create<atlas_msgs::AtlasState>("/atlas/atlas_state", 10, &AtlasStateCallback, ros::VoidPtr(), nh.getCallbackQueue());
  jointStatesSo.transport_hints = ros::TransportHints().reliable().tcpNoDelay(true);
  sub_atlas_state = nh.subscribe(jointStatesSo); 
  //////////////////////////////////////////////////////////////
  // tell the simulator that I have control now
  ros::Publisher pub_bdi_asi_cmd = nh.advertise<atlas_msgs::AtlasSimInterfaceCommand> 
    ( "/atlas/atlas_sim_interface_command", 1);
  atlas_msgs::AtlasSimInterfaceCommand bdi_cmd;
  bdi_cmd.header.stamp = ros::Time::now();
  bdi_cmd.behavior = atlas_msgs::AtlasSimInterfaceCommand::USER;
  pub_bdi_asi_cmd.publish(bdi_cmd);
  ////////////////////////////////////////////////////////////// 

  int cState;
  CMUCtrlUtils utils;           // handles talking to the simulator
  PelvRobotState rs;            // contains all info about robot
  //HatRobotState rs;             // contains all info about robot
  KinematicFilter3 kcekf;       // state estimator
  LipmWalkingCon lwc;           // walking controller
  Command cmd;                  // contains outputs from controller

  bool inited = false;
  double init_time = 0;
  double last_rec_time = 0;

  // initialize controller
  load_KFParams(std::string("test_walk"), kcekf);
  lwc.allocCon(rs.getType());
  // load control params
  load_sf_params(
      std::string("test_walk"),
      std::string("/config/con_param/atlas_static_idCon.conf"),
      std::string("/config/con_param/atlas_static_ikCon.conf"),
      std::string("/config/con_param/atlas_wc.conf"),
      lwc); 

  double t0, t1;
  double dt[3];

  logger.init(0.001);
  rs.addToLog(logger);
  cmd.addToLog(logger);
  lwc.idcmd.addToLog(logger);
  lwc.idCon->addToLog(logger);
  lwc.ikcmd.addToLog(logger);
  lwc.ikCon->addToLog(logger);
  utils.addToLog(logger);
  lwc.addToLog(logger); 
  logger.add_datapoint("dt_est", "-", dt);
  logger.add_datapoint("dt_qp", "-", dt+1);
  logger.add_datapoint("dt_send", "-", dt+2);

  int state = 0;


  // main loop
  while (1) 
  {
    // tell ros to process all listening / publishing callbacks
    ros::spinOnce();

    // process data from simulator
    {
      boost::mutex::scoped_lock lock(state_lock);
      // haven't got the first real robot state from simulator
      if (data_from_robot.header.stamp.toSec() == 0)
        continue;
      
      // haven't got a new packet
      if (data_from_robot.header.stamp.toSec() == last_rec_time)
        continue;

      utils.UnpackDataFromRobot(data_from_robot);
      last_rec_time = data_from_robot.header.stamp.toSec();
    }

    t0 = get_time();    

    // initialize state estimator to the first real robot state
    if (!inited) {
      for (int i = 0; i < N_JOINTS; i++)
        utils.f_mask[i] = CMUCtrlUtils::FF;
      utils.init_KF(kcekf, rs.getType(), 0.9545);
      utils.updateRobotState(DSc, rs);
      lwc.init(rs);
      inited = true;
      init_time = utils.time;
      rs.ctrl_dt = 0;
    }
    // run state estimator and update robot state normally
    else {
      cState = lwc.getPlannedContactState(utils.time);
      utils.estimateState(cState, kcekf, utils.foot_forces[LEFT][ZZ], utils.foot_forces[RIGHT][ZZ]);
      utils.updateRobotState(cState, rs);
    }
    
    if (rs.time - init_time > 5 && state == 0) {
      state++;
      std::vector<SFootStep> fsplan;
      double pos0[6], pos1[6];
      dvec_copy(pos0, rs.root, 6); 
      pos0[ZZ] = (rs.feet[LEFT].w_mid_pos[ZZ]+rs.feet[RIGHT].w_mid_pos[ZZ]) / 2.; 
      dvec_copy(pos1, pos0, 6); 
      pos1[XX] = 3.5; 
      make_straight_fs_plan(pos0, pos1, 0.3, fsplan);
      lwc.updateFootSteps(rs, fsplan);
    }

    t1 = get_time();
    dt[0] = t1 - t0;
    t0 = t1;

    // run walking controller
    lwc.control(rs, cmd);
    
    t1 = get_time();
    dt[1] = t1 - t0;
    t0 = t1;

    utils.PackDataToRobot(cmd, rs.time, data_to_robot);
    logger.saveData();

    // send commands to simulator
    pub_atlas_cmd.publish(data_to_robot);

    t1 = get_time();
    dt[2] = t1 - t0;
    t0 = t1;
  }    

  quit(SIGINT);
}




static void quit(int sig)
{
  ros::shutdown();
  ros::waitForShutdown(); 

  logger.writeToMRDPLOT();
  printf( "Goodbye\n" );
  exit(0);  
}

