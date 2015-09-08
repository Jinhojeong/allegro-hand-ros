/*
 * allegroNode.cpp
 *
 *  Created on: Nov 14, 2012
 *  Authors: Alex ALSPACH, Seungsu KIM
 */

// 20141210: kcchang: changed Duration to accomodate the hand's own CAN rate
// 20141211: kcchang: merged callback and polling

// GRASP LIBRARY INTERFACE
// Using  timer callback

#include <iostream>
#include <boost/thread/thread.hpp>
#include <boost/date_time.hpp>
#include <boost/thread/locks.hpp>

#include "ros/ros.h"
#include "ros/service.h"
#include "ros/service_server.h"
#include "sensor_msgs/JointState.h"
#include "std_msgs/String.h"
#include "std_msgs/Float32.h"

#include <stdio.h>
#include <iostream>
#include <string>

#include "BHand/BHand.h"
#include "allegro_hand_common/controlAllegroHand.h"


// Topics
#define JOINT_STATE_TOPIC "/allegroHand/joint_states"
#define JOINT_CMD_TOPIC "/allegroHand/joint_cmd"
#define LIB_CMD_TOPIC "/allegroHand/lib_cmd"
#define ENVELOP_TORQUE_TOPIC "/allegroHand/envelop_torque"

double desired_position[DOF_JOINTS] 			= {0.0};
double current_position[DOF_JOINTS] 			= {0.0};
double previous_position[DOF_JOINTS] 			= {0.0};
double current_position_filtered[DOF_JOINTS] 	= {0.0};
double previous_position_filtered[DOF_JOINTS]	= {0.0};

double current_velocity[DOF_JOINTS] 			= {0.0};
double previous_velocity[DOF_JOINTS] 			= {0.0};
double current_velocity_filtered[DOF_JOINTS] 	= {0.0};

double desired_torque[DOF_JOINTS] 				= {0.0};

std::string jointNames[DOF_JOINTS] 	=
{
  "joint_0.0",    "joint_1.0",    "joint_2.0",   "joint_3.0" ,
  "joint_4.0",    "joint_5.0",    "joint_6.0",   "joint_7.0" ,
  "joint_8.0",    "joint_9.0",    "joint_10.0",  "joint_11.0",
  "joint_12.0",   "joint_13.0",   "joint_14.0",  "joint_15.0"
};

long frame = 0;

// Flags
int lEmergencyStop = 0;

boost::mutex *mutex;

// ROS Messages
ros::Publisher joint_state_pub;
ros::Subscriber joint_cmd_sub;
ros::Subscriber lib_cmd_sub;
ros::Subscriber envelop_torque_sub;
sensor_msgs::JointState msgJoint;

// ROS Time
ros::Time tstart;
ros::Time tnow;
double dt;

// Initialize BHand
BHand* pBHand = NULL;

// Initialize CAN device
controlAllegroHand* canDevice;

// Called when a desired joint position message is received
void SetjointCallback(const sensor_msgs::JointState& msg)
{
  //	printf("frame = %ld: setting desired pos\n", frame);

  // TODO check joint limits

  mutex->lock();
  for (int i=0; i<DOF_JOINTS; i++) desired_position[i] = msg.position[i];
  mutex->unlock();

  pBHand->SetJointDesiredPosition(desired_position);
  pBHand->SetMotionType(eMotionType_JOINT_PD);
}

// The grasp controller can set the desired envelop grasp torque by listening to
// Float32 messages on ENVELOP_TORQUE_TOPIC ("/allegroHand/envelop_torque").
void envelopTorqueCallback(const std_msgs::Float32& msg) {
  const double torque = msg.data;
  ROS_INFO("Setting envelop torque to %.3f.", torque);
  pBHand->SetEnvelopTorqueScalar(torque);
}

// BHAND Communication
// desired joint positions are obtained from subscriber "joint_cmd_sub"
// or maintatined as the initial positions from program start (PD control)
// Also, other motions/grasps can be executed (envoked via "lib_cmd_sub")

/*
  eMotionType_NONE,				// motor power off
  eMotionType_HOME,				// go to home position
  eMotionType_READY,			// ready position for grasping
  eMotionType_GRASP_3,			// grasping using 3 fingers
  eMotionType_GRASP_4,			// grasping using 4 fingers
  eMotionType_PINCH_IT,			// pinching using index finger and thumb
  eMotionType_PINCH_MT,			// pinching using middle finger and thumb
  eMotionType_ENVELOP,			// enveloping
  eMotionType_JOINT_PD,			// joint pd control
  eMotionType_OBJECT_MOVING,	//
  eMotionType_PRE_SHAPE,		//
*/

// Define a map from string (received message) to eMotionType (Bhand controller grasp).
std::map<std::string, eMotionType> bhand_grasps = {
        {"home", eMotionType_HOME},
        {"home", eMotionType_HOME},
        {"ready", eMotionType_READY},
        {"grasp_3", eMotionType_GRASP_3},
        {"grasp_4", eMotionType_GRASP_4},
        {"pinch_it", eMotionType_PINCH_IT},
        {"pinch_mt", eMotionType_PINCH_MT},
        {"envelop", eMotionType_ENVELOP},
        {"off", eMotionType_NONE}
};

void libCmdCallback(const std_msgs::String::ConstPtr& msg) {
  ROS_INFO("CTRL: Heard: [%s]", msg->data.c_str());
  const std::string lib_cmd = msg->data;

  // Main behavior: apply the grasp directly from the map. Secondary behaviors can still be handled
  // normally (case-by-case basis), note these should *not* be in the map.
  auto itr = bhand_grasps.find(msg->data);
  if(itr != bhand_grasps.end()) {
    pBHand->SetMotionType(itr->second);
  } else if (lib_cmd.compare("pdControl") == 0) {
    // Desired position only necessary if in PD Control mode
    pBHand->SetJointDesiredPosition(desired_position);
    pBHand->SetMotionType(eMotionType_JOINT_PD);
  } else if (lib_cmd.compare("save") == 0) {
    for (int i = 0; i < DOF_JOINTS; i++)
      desired_position[i] = current_position[i];
  } else {
    ROS_WARN("Unknown commanded grasp: %s.", lib_cmd.c_str());
  }
}

void computeDesiredTorque()
{
  // compute control torque using Bhand library
  pBHand->SetJointPosition(current_position_filtered);

  // BHand lib control updated with time stamp
  pBHand->UpdateControl((double)frame*ALLEGRO_CONTROL_TIME_INTERVAL);

  // Necessary torque obtained from Bhand lib
  pBHand->GetJointTorque(desired_torque);
}

void initController(const std::string& whichHand)
{
  // Initialize BHand controller
  if (whichHand.compare("left") == 0)
  {
    pBHand = new BHand(eHandType_Left);
    ROS_WARN("CTRL: Left Allegro Hand controller initialized.");
  }
  else
  {
    pBHand = new BHand(eHandType_Right);
    ROS_WARN("CTRL: Right Allegro Hand controller initialized.");
  }
  pBHand->SetTimeInterval(ALLEGRO_CONTROL_TIME_INTERVAL);
  pBHand->SetMotionType(eMotionType_NONE);

  // sets initial desired pos at start pos for PD control
  for (int i=0; i<DOF_JOINTS; i++) desired_position[i] = current_position[i];

  printf("*************************************\n");
  printf("         Grasp (BHand) Method        \n");
  printf("-------------------------------------\n");
  printf("         Every command works.        \n");
  printf("*************************************\n");
}

void cleanController()
{
  delete pBHand;
}

void publishData()
{
  // current position, velocity and effort (torque) published
  msgJoint.header.stamp = tnow;
  for (int i=0; i<DOF_JOINTS; i++)
  {
    msgJoint.position[i] = current_position_filtered[i];
    msgJoint.velocity[i] = current_velocity_filtered[i];
    msgJoint.effort[i] = desired_torque[i];
  }
  joint_state_pub.publish(msgJoint);
}

void updateWriteReadCAN()
{
  /* ==================================
     =        CAN COMMUNICATION         =
     ================================== */
  canDevice->setTorque(desired_torque);
  lEmergencyStop = canDevice->Update();
  canDevice->getJointInfo(current_position);

  if (lEmergencyStop < 0)
  {
    // Stop program when Allegro Hand is switched off
    ROS_ERROR("Allegro Hand Node is Shutting Down! (Emergency Stop)");
    ros::shutdown();
  }
}

void updateController()
{
  // Calculate loop time;
  tnow = ros::Time::now();
  dt = 1e-9*(tnow - tstart).nsec;
  tstart = tnow;

  // save last iteration info
  for (int i=0; i<DOF_JOINTS; i++)
  {
    previous_position[i] = current_position[i];
    previous_position_filtered[i] = current_position_filtered[i];
    previous_velocity[i] = current_velocity[i];
  }

  updateWriteReadCAN();

  /* ==================================
     =         LOWPASS FILTERING        =
     ================================== */
  for (int i=0; i<DOF_JOINTS; i++)
  {
    current_position_filtered[i] = (0.6*current_position_filtered[i]) + (0.198*previous_position[i]) + (0.198*current_position[i]);
    current_velocity[i] = (current_position_filtered[i] - previous_position_filtered[i]) / dt;
    current_velocity_filtered[i] = (0.6*current_velocity_filtered[i]) + (0.198*previous_velocity[i]) + (0.198*current_velocity[i]);
    current_velocity[i] = (current_position[i] - previous_position[i]) / dt;
  }

  computeDesiredTorque();

  publishData();

  frame++;
}

// In case of the Allegro Hand, this callback is processed every 0.003 seconds
void timerCallback(const ros::TimerEvent& event)
{
  updateController();
}

int main(int argc, char** argv)
{
  using namespace std;

  ros::init(argc, argv, "allegro_hand_core_grasp");
  ros::Time::init();

  ros::NodeHandle nh;

  mutex = new boost::mutex();

  // Publisher and Subscribers
  joint_state_pub = nh.advertise<sensor_msgs::JointState>(JOINT_STATE_TOPIC, 3);
  joint_cmd_sub = nh.subscribe(JOINT_CMD_TOPIC, 3, SetjointCallback);
  lib_cmd_sub = nh.subscribe(LIB_CMD_TOPIC, 1, libCmdCallback);
  envelop_torque_sub = nh.subscribe(ENVELOP_TORQUE_TOPIC, 1, envelopTorqueCallback);

  // Create arrays 16 long for each of the four joint state components
  msgJoint.position.resize(DOF_JOINTS);
  msgJoint.velocity.resize(DOF_JOINTS);
  msgJoint.effort.resize(DOF_JOINTS);
  msgJoint.name.resize(DOF_JOINTS);

  // Joint names (for use with joint_state_publisher GUI - matches URDF)
  for (int i=0; i<DOF_JOINTS; i++)	msgJoint.name[i] = jointNames[i];

  // Get Allegro Hand information from parameter server
  // This information is found in the Hand-specific "zero.yaml" file from the allegro_hand_description package
  string robot_name, whichHand, manufacturer, origin, serial;
  double version;
  ros::param::get("~hand_info/robot_name",robot_name);
  ros::param::get("~hand_info/which_hand",whichHand);
  ros::param::get("~hand_info/manufacturer",manufacturer);
  ros::param::get("~hand_info/origin",origin);
  ros::param::get("~hand_info/serial",serial);
  ros::param::get("~hand_info/version",version);

  // Dump Allegro Hand information to the terminal
  cout << endl << endl << robot_name << " v" << version << endl << serial << " (" << whichHand << ")" << endl << manufacturer << endl << origin << endl << endl;

  // Initialize CAN device
  canDevice = new controlAllegroHand();
  canDevice->init();
  usleep(3000);

  // Initialize torque at zero
  for (int i=0; i<DOF_JOINTS; i++) desired_torque[i] = current_velocity[i] = 0.0;

  // Initialize current pos to Hand pos
  updateWriteReadCAN();

  initController(whichHand);

  // Start ROS time
  tstart = ros::Time::now();

  // Starts control loop, message pub/subs and all other callbacks
  ROS_INFO("Start controller with polling = %s", argv[1]);

  if (argv[1] == std::string("true")) //polling:=true
  {
    while (ros::ok())
    {
      updateController();
      ros::spinOnce();
    }
  }
  else
  {
    // Setup timer callback
    ros::Timer timer = nh.createTimer(ros::Duration(0.001), timerCallback); //KCX
    ros::spin();
  }

  // Prompt user to press enter to quit node for viewing failed launch or system after CAN error
  //cout << "Press Enter to Continue";
  //cin.ignore();

  // Clean shutdown: shutdown node, shutdown can, be polite.
  nh.shutdown();
  cleanController();
  delete canDevice;
  ROS_INFO("Allegro Hand Node has been shut down.");
  return 0;
}
