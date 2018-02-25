#include <koko_controllers/InverseDynamicsController.h>
#include <pluginlib/class_list_macros.h>
#include <angles/angles.h>
#include <kdl/frames.hpp>
#include <vector>
#include <iterator>
#include <unistd.h>
#include <algorithm>
#include <math.h>
#include <numeric>
#include <geometry_msgs/Vector3.h>
namespace koko_controllers{

  InverseDynamicsController::~InverseDynamicsController() {
    for (int i = 0; i < joint_vector.size(); i++) {
      delete joint_vector[i];
    }
  }

  bool InverseDynamicsController::init(hardware_interface::EffortJointInterface *robot, ros::NodeHandle &n)
  {

    ros::NodeHandle node;
    std::string robot_desc_string;
    if (!node.getParam("robot_dyn_description", robot_desc_string)) {
      ROS_ERROR("No robot_dyn_description given, %s", node.getNamespace().c_str());
    }

    KDL::Tree my_tree;

    if(!kdl_parser::treeFromString(robot_desc_string, my_tree)){
      ROS_ERROR("Failed to contruct kdl tree");
      return false;
    }

    std::string endlink;
    if (!n.getParam("endlink", endlink)) {
      ROS_ERROR("No endlink given node namespace %s", n.getNamespace().c_str());
    }

    if (!n.getParam("zero_g_mode", zero_g_mode)) {
      ROS_ERROR("No zero_g_mode given node namespace %s", n.getNamespace().c_str());
    }

    if (!n.getParam("paired_constraints", paired_constraints)) {
      ROS_ERROR("No paired_constraint given node namespace %s", n.getNamespace().c_str());
    }

    if (paired_constraints.size() % 2 != 0) {
      ROS_ERROR("Paired_constraints length must be even");
    }

    KDL::Chain dummyChain;

    if (!my_tree.getChain("base_link", endlink, dummyChain)) {
      ROS_ERROR("Failed to construct kdl chain");
      return false;
    }
    int filter_length;
    if (!n.getParam("filter_length", filter_length)) {
      ROS_ERROR("No filter_length given node namespace %s", n.getNamespace().c_str());
    }

    int ns = dummyChain.getNrOfSegments();
    ROS_INFO("ns = %d", ns);
    gravity.data[0] = 0;
    gravity.data[1] = 0;
    gravity.data[2] = -9.81;

    for(int i = 0; i < ns; i++){
      KDL::Segment seg = dummyChain.segments[i];

      if (seg.getJoint().getType() != 8)
      {
        JointPD* jointPD = new JointPD();
        std::string jointName = seg.getJoint().getName();
        joint_names.push_back(jointName);
        chain.addSegment(seg);
        jointPD->joint = robot->getHandle(jointName);
        double p_gain;
        double d_gain;
        double id_gain;
        double max_torque;
        double min_torque;
        double min_angle;
        double max_angle;
        if (!n.getParam(jointName + "/p", p_gain)) {
          ROS_ERROR("No %s/p given (namespace: %s)", jointName.c_str(), n.getNamespace().c_str());
          return false;
        }
        if (!n.getParam(jointName + "/d", d_gain)) {
          ROS_ERROR("No %s/d given (namespace: %s)", jointName.c_str(), n.getNamespace().c_str());
          return false;
        }
        if (!n.getParam(jointName + "/id", id_gain)) {
          ROS_ERROR("No %s/i given (namespace: %s)", jointName.c_str(), n.getNamespace().c_str());
          return false;
        }
        if (!n.getParam(jointName + "/max_torque", max_torque)) {
          ROS_ERROR("No %s/max_torque given (namespace: %s)", jointName.c_str(), n.getNamespace().c_str());
          return false;
        }
        if (!n.getParam(jointName + "/min_torque", min_torque)) {
          ROS_ERROR("No %s/min_torque given (namespace: %s)", jointName.c_str(), n.getNamespace().c_str());
          return false;
        }
        if (!n.getParam(jointName + "/min_angle", min_angle)) {
          ROS_ERROR("No %s/min_angle given (namespace: %s)", jointName.c_str(), n.getNamespace().c_str());
          return false;
        }
        if (!n.getParam(jointName + "/max_angle", max_angle)) {
          ROS_ERROR("No %s/max_angle given (namespace: %s)", jointName.c_str(), n.getNamespace().c_str());
          return false;
        }

        jointPD->max_torque = max_torque;
        jointPD->min_torque = min_torque;
        jointPD->max_angle = max_angle;
        jointPD->min_angle = min_angle;
        jointPD->p_gain = p_gain;
        jointPD->d_gain = d_gain;
        jointPD->id_gain = id_gain;
        jointPD->joint_name = jointName;
        jointPD->err_dot_filter_length = filter_length;
        joint_vector.push_back(jointPD);

        ROS_INFO("Joint %s, has inverse dynamics gain of: %f", jointName.c_str(), id_gain);

      }
    }

    std::string root_name;
    if (!n.getParam("root_name", root_name)) {
      ROS_ERROR("No root_name given (namespace: %s)", n.getNamespace().c_str());
      return false;
    }

    id_torques = KDL::JntArray(chain.getNrOfJoints());
    commandPub = node.advertise<std_msgs::Float64MultiArray>("commandPub", 1);
    deltaPub = node.advertise<std_msgs::Float64MultiArray>("deltaPub", 1);
    inverseDynamicsPub = node.advertise<std_msgs::Float64MultiArray>("inverseDynamicsPub", 1);

    sub_command = n.subscribe("command", 1, &InverseDynamicsController::setCommand, this);
    //sub_joint = node.subscribe("/" + root_name  + "/joint_states", 1000, &InverseDynamicsController::jointCallback, this);
    sub_joint = node.subscribe( "/joint_states", 1000, &InverseDynamicsController::jointCallback, this);
    sub_grav = node.subscribe( "/koko_hardware/gravity", 1000, &InverseDynamicsController::gravCallback, this);

    sub_p = node.subscribe( "/p_terms", 1000, &InverseDynamicsController::pCallback, this);
    sub_d = node.subscribe( "/d_terms", 1000, &InverseDynamicsController::dCallback, this);
    return true;
  }

  void InverseDynamicsController::gravCallback(const geometry_msgs::Vector3ConstPtr& grav) {
    gravity[0] = grav->x;
    gravity[1] = grav->y;
    gravity[2] = grav->z;
  }

  void InverseDynamicsController::pCallback(const std_msgs::Float64MultiArrayConstPtr& p_terms) {
    std::vector<double> p = p_terms->data;
    for(int i = 0; i < p.size(); i++){
       ROS_ERROR("Updateing p gain of joint %d, to %f", i, p[i]);
       joint_vector[i]->p_gain = p[i];
    }
  }
  void InverseDynamicsController::dCallback(const std_msgs::Float64MultiArrayConstPtr& d_terms) {
    std::vector<double> d = d_terms->data;
    for(int i = 0; i < d.size(); i++){
       ROS_ERROR("Updateing d gain of joint %d, to %f", i, d[i]);
       joint_vector[i]->d_gain = d[i];
    }
  }

  void InverseDynamicsController::starting(const ros::Time& time)
  {
    for (int i = 0; i < joint_vector.size(); i++) {
      joint_vector[i]->cmd = joint_vector[i]->joint.getPosition();

      //ROS_INFO("command start %f", joint_vector[i].cmd);
    }
  }


  void InverseDynamicsController::setCommand(const std_msgs::Float64MultiArrayConstPtr& pos_commands) {
    std::vector<double> commands = pos_commands->data;

    for (int i = 0; i < commands.size(); i++)  {
      //account for wrap around
      commands[i] = fmod(commands[i] + 3.14159265359, 6.28318530718);
      if (commands[i] < 0) commands[i] += 6.28318530718;
      commands[i] = commands[i] - 3.1415926535;
      commands[i] = std::min(std::max(commands[i], joint_vector[i]->min_angle), joint_vector[i]->max_angle);

      joint_vector[i]->cmd = commands[i];
    }

  }

  void InverseDynamicsController::update(const ros::Time& time, const ros::Duration& period)
  {
    std_msgs::Float64MultiArray commandMsg;
    for (int i = 0; i < joint_vector.size(); i++) {
      commandMsg.data.push_back(joint_vector[i]->cmd);
    }
    commandPub.publish(commandMsg);

    std_msgs::Float64MultiArray deltaMsg;

    std::vector<double> commands(joint_vector.size());
    for (int i = 0; i < joint_vector.size(); i++) {
      double current_position = joint_vector[i]->joint.getPosition();
      double current_velocity = joint_vector[i]->joint.getVelocity();
      double error = angles::shortest_angular_distance(current_position, joint_vector[i]->cmd); // TODO: don't use wraparound here
      deltaMsg.data.push_back(error);
      //ROS_INFO("Joint pos %f, joint cmd %f", current_position, joint_vector[i].cmd);
      //ROS_INFO("Joint %d error: %f", i, error);
      double commanded_effort = computeCommand(error, period, i, current_velocity);

      //ignore min max torque for lift roll pairs
      if(std::find(paired_constraints.begin(), paired_constraints.end(), i) == paired_constraints.end()) {
        commanded_effort = std::min(std::max(commanded_effort, joint_vector[i]->min_torque), joint_vector[i]->max_torque);
      }
      commands[i] = commanded_effort;
    }
    deltaPub.publish(deltaMsg);


    //ROS_INFO("joint name order: %s, %s, %s, %s", joint_vector[0].joint_name.c_str(), joint_vector[1].joint_name.c_str(), joint_vector[2].joint_name.c_str(), joint_vector[3].joint_name.c_str());
    //ROS_INFO("current positions: %f, %f, %f, %f", joint_vector[0].joint.getPosition(), joint_vector[1].joint.getPosition(), joint_vector[2].joint.getPosition(), joint_vector[3].joint.getPosition());
    //ROS_INFO("inverse dynamics torques: %f %f %f", id_torques(0), id_torques(1), id_torques(2));



    for (int i = 0; i < paired_constraints.size(); i = i + 2) {
      int lift_index = paired_constraints[i];
      int roll_index = paired_constraints[i+1];
      double lift = commands[lift_index];
      double roll = commands[roll_index];
      //ROS_INFO("lift: %f, roll: %f", commands[lift_index],  commands[roll_index]);

      double max_effort = joint_vector[lift_index]->max_torque + joint_vector[roll_index]->max_torque;
      double scalar = 1;
      if (lift >= 0 && roll >= 0 && (lift + roll) > max_effort) {
        scalar = max_effort / (lift + roll);
      } else if (lift < 0 && roll >= 0 && (roll - lift) > max_effort) {
        scalar = max_effort / (roll - lift);
      } else if (lift < 0 && roll < 0 && (-roll - lift) > max_effort) {
        scalar = max_effort / (-roll - lift);
      } else if (lift >= 0 && roll < 0 && (lift - roll) > max_effort) {
        scalar = max_effort / (lift - roll);
      }
      commands[lift_index] *= scalar;
      commands[roll_index] *= scalar;
    }
    for (int i = 0; i < joint_vector.size(); i++) {
      // ROS_INFO("command %d: %f", i, commands[i]);
      joint_vector[i]->joint.setCommand(commands[i]);
    }
  }

  double InverseDynamicsController::computeCommand(double error, ros::Duration dt, int index, double vel)
  {
    if (dt == ros::Duration(0.0) || std::isnan(error) || std::isinf(error))
      return 0.0;

    // double error_dot = joint_vector[index].d_error;
    // if (dt.toSec() > 0.0)  {
    //   error_dot = (error - joint_vector[index].p_error_last) / dt.toSec();
    //   joint_vector[index].p_error_last = error;
    // }
    // if (std::isnan(error_dot) || std::isinf(error_dot))
    //   return 0.0;
    double p_term, d_term;
    // joint_vector[index].d_error = error_dot;
    // joint_vector[index].err_dot_history.push_back(error_dot);
    // if (joint_vector[index].err_dot_history.size() > joint_vector[index].err_dot_filter_length) {
    //   joint_vector[index].err_dot_history.erase(joint_vector[index].err_dot_history.begin());
    // }
    // double err_dot_average = std::accumulate(joint_vector[index].err_dot_history.begin(),
    //                             joint_vector[index].err_dot_history.end(),
    //                             0.0) / joint_vector[index].err_dot_history.size();
    // //ROS_INFO("error_dot_average %f", err_dot_average);
    // ROS_ERROR("%f %d, error difference and index", vel+err_dot_average, index);
    p_term = joint_vector[index]->p_gain * error;
    d_term = joint_vector[index]->d_gain * -vel;

    //ROS_DEBUG("inverse dynamics torques %f", id_torques(index));
    if (zero_g_mode){
      // ROS_ERROR("ZERO GRAVITY MODE");
      return id_torques(index) * joint_vector[index]->id_gain;
    }
    //ROS_INFO("d_term Torques: %f", d_term);
    return (p_term + d_term) + id_torques(index) * joint_vector[index]->id_gain;
    //return p_term + d_term + 0.3 * id_torques(index);
  }

  void InverseDynamicsController::jointCallback(const sensor_msgs::JointState msg)
  {
    unsigned int nj = chain.getNrOfJoints();
    KDL::JntArray jointPositions(nj);
    KDL::JntArray jointVelocities(nj);
    KDL::JntArray jointAccelerations(nj);
    KDL::Wrenches f_ext;
    for (int i = 0; i < nj; i++) {
      for (int index = 0; index < nj; index++) {
        if (msg.name[i].compare(joint_names[index]) == 0) {
          jointPositions(index) = msg.position[i];
          jointVelocities(index) = msg.velocity[i];
          jointAccelerations(index) = 0.0;
          f_ext.push_back(KDL::Wrench());

          break;
        } else if (index == nj - 1){
           ROS_ERROR("No joint %s for controller", msg.name[i].c_str());
        }
      }
    }

    KDL::ChainIdSolver_RNE chainIdSolver(chain, gravity);
    int statusID = chainIdSolver.CartToJnt(jointPositions, jointVelocities, jointAccelerations, f_ext, id_torques);
    // ROS_INFO("status: %d", statusID);
    //ROS_INFO("pos vel =  %f, %f", msg.position[0], msg.velocity[0]);


    std_msgs::Float64MultiArray inverseDynamicsMsg;
    for (int i = 0; i < joint_vector.size(); i++) {
      inverseDynamicsMsg.data.push_back(id_torques(i));
    }
    inverseDynamicsPub.publish(inverseDynamicsMsg);
  }

}

PLUGINLIB_EXPORT_CLASS(koko_controllers::InverseDynamicsController, controller_interface::ControllerBase)
