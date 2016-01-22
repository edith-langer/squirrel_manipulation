#include "../include/PushPlanner.hpp"

#include<iostream>
#include<armadillo>

using namespace std;
using namespace arma;

int sgn(double d){
    return d<0 ? -1:1;
}

PushPlanner::PushPlanner()
{
    this->push_state_ = INACTIVE;
    this->push_active_ = false;
}

PushPlanner::PushPlanner(string local_frame_, string global_frame_, geometry_msgs::Pose2D pose_robot_, geometry_msgs::PoseStamped pose_object_, nav_msgs::Path pushing_path_, double lookahead_, double goal_toll_, bool state_machine_, double controller_frequency_, double object_diameter_):
    private_nh("~")
{
    this->initialize(local_frame_, global_frame_, pose_robot_, pose_object_, pushing_path_, lookahead_, goal_toll_, state_machine_, controller_frequency_, object_diameter_);

}

void PushPlanner::initialize(string local_frame_, string global_frame_, geometry_msgs::Pose2D pose_robot_, geometry_msgs::PoseStamped pose_object_, nav_msgs::Path pushing_path_, double lookahead_, double goal_toll_, bool state_machine_, double controller_frequency_, double object_diameter_){

    this->global_frame_ = global_frame_;
    this->local_frame_ = local_frame_;
    this->pose_robot_ = pose_robot_;
    this->pose_object_ = pose_object_;
    this->pushing_path_ = pushing_path_;
    this->goal_reached_ = false;
    this->push_active_ = false;
    this->goal_toll_ = goal_toll_;
    this->state_machine_ = state_machine_;
    this->controller_frequency_ = controller_frequency_;
    this->object_diameter_= object_diameter_;
    this->time_step_ = 1 / controller_frequency_;
    this->push_state_ = INACTIVE;
    this->goal_ = pushing_path_.poses[pushing_path_.poses.size() - 1];

    vis_points_pub_ = nh.advertise<visualization_msgs::MarkerArray>("/push_action/push_markers", 10, true);
    marker_target_c_ = nh.advertise<visualization_msgs::Marker>("/push_action/current_target", 10, true);
    marker_object_c_ = nh.advertise<visualization_msgs::Marker>("/push_action/current_object_pose", 10, true);
    marker_point_ = nh.advertise<visualization_msgs::Marker>("/push_action/point", 10, true);
    visualise_ = false;

    pose_robot_vec_.set_size(pushing_path_.poses.size(), 3);
    pose_object_vec_.set_size(pushing_path_.poses.size(), 3);
    current_target_vec_.set_size(pushing_path_.poses.size(), 3);
    elem_count_ = 0;

    setLookahedDistance(lookahead_);

    this->initChild();
}

void PushPlanner::updatePushPlanner(geometry_msgs::Pose2D pose_robot_, geometry_msgs::PoseStamped pose_object_){

    this->pose_robot_ = pose_robot_;
    this->pose_object_ = pose_object_;
    this->current_target_ = this->getLookaheadPoint();

    this->updateMatrix();

    //the angle of a vector object-target point
    aO2P = getVectorAngle(pose_object_.pose.position.y - current_target_.pose.position.y, pose_object_.pose.position.x - current_target_.pose.position.x);

    //the angle of a vector robot-object
    aR2O = getVectorAngle(pose_robot_.y - pose_object_.pose.position.y, pose_robot_.x - pose_object_.pose.position.x);

    //the angle object-robot-target
    aORT =  angle3Points(pose_object_.pose.position.x, pose_object_.pose.position.y, pose_robot_.x, pose_robot_.y, current_target_.pose.position.x, current_target_.pose.position.y);

    //translation error object-goal
    double dO2G = distancePoints(pose_object_.pose.position.x, pose_object_.pose.position.y, goal_ .pose.position.x, goal_.pose.position.y);

    //distance object target point
    dO2P = distancePoints(pose_object_.pose.position.x, pose_object_.pose.position.y, current_target_.pose.position.x, current_target_.pose.position.y);

    //distance robot object
    dR2O = distancePoints(pose_robot_.x, pose_robot_.y, pose_object_.pose.position.x, pose_object_.pose.position.y);

    switch (push_state_){

    case RELOCATE:
    {
        if(!rel_){
            relocate_target_vec_.set_size(3,1);
            relocate_target_.set_size(3);

            rel_ = true;
        }

        relocate_target_vec_(span(0,1),relocate_target_vec_.n_cols - 1) = reflectPointOverPoint(pose_object_.pose.position.x, pose_object_.pose.position.y,  current_target_.pose.position.x, current_target_.pose.position.y);
        relocate_target_vec_(2, relocate_target_vec_.n_cols - 1) = aO2P;
        for (int i = 0; i < 3; i ++) relocate_target_(i) = mean(relocate_target_vec_.row(i));
        relocate_target_vec_.resize(3,relocate_target_vec_.n_cols + 1);

        publishPoint(relocate_target_);

        if ((distancePoints(pose_robot_.x, pose_robot_.y, relocate_target_ (0), relocate_target_ (1)) < 0.06) && (rotationDifference(relocate_target_(2), pose_robot_.theta) < 0.1) && (dR2O < 2 * object_diameter_))
            push_state_ = PUSH;

        if((fabs(aO2P - aR2O) < 0.3) && (fabs(rotationDifference(aR2O, pose_robot_.theta)) < 0.3))
            push_state_ = APPROACH;

    }
        break;

    case APPROACH:
    {
        if (dR2O < 1.2 * object_diameter_)
            push_state_ = PUSH;

    }
        break;

    case PUSH:
    {

        if (dO2G < goal_toll_){
            goal_reached_ = true;
            push_state_ = INACTIVE;
        }

    }
        break;

    case INACTIVE:
    {
        push_active_ = false;

    }
        break;

    }


}

geometry_msgs::PoseStamped PushPlanner::getLookaheadPoint(){

    //getting the closests point on path
    int p_min_ind = 0;
    double d_min = std::numeric_limits<double>::infinity();

    for(size_t i = 0; i < pushing_path_.poses.size(); i++) {

        double d_curr = distancePoints(pushing_path_.poses[i].pose.position.x, pushing_path_.poses[i].pose.position.y, pose_object_.pose.position.x, pose_object_.pose.position.y);
        if (d_min > d_curr){
            p_min_ind = i;
            d_min = d_curr;
        }
    }

    //determining the target point with lookahead distance
    double neighbourhood_min = std::numeric_limits<double>::infinity();
    int p_lookahead = p_min_ind;

    if(distancePoints(pushing_path_.poses[pushing_path_.poses.size() - 1].pose.position.x, pushing_path_.poses[pushing_path_.poses.size() - 1].pose.position.y, pushing_path_.poses[p_min_ind].pose.position.x, pushing_path_.poses[p_min_ind].pose.position.y) < lookahead_){
        p_lookahead = pushing_path_.poses.size() - 1;
    }
    else{

        for (size_t i = p_min_ind; i < pushing_path_.poses.size(); i++) {

            double d_curr = distancePoints(pushing_path_.poses[i].pose.position.x, pushing_path_.poses[i].pose.position.y, pushing_path_.poses[p_min_ind].pose.position.x, pushing_path_.poses[p_min_ind].pose.position.y);
            if (abs(d_curr - lookahead_) < neighbourhood_min){
                neighbourhood_min = abs(d_curr - lookahead_);
                p_lookahead = i;
            }
        }
    }

    if (visualise_){
        publishMarkerTargetCurrent(pushing_path_.poses[p_lookahead]);
        publishMarkerObjectCurrent(pose_object_);

    }

    return pushing_path_.poses[p_lookahead];
}

void PushPlanner::setLookahedDistance(double d){

    lookahead_ = d;
}

void PushPlanner::startPush(){

    if (push_state_ == INACTIVE)
        push_state_ = RELOCATE;



    push_active_ = true;
    goal_reached_ = false;

    rel_ = false;

}

void PushPlanner::stopPush(){

    push_state_ = INACTIVE;
    push_active_  = false;
}

geometry_msgs::Twist PushPlanner::getControlCommand(){
    geometry_msgs::Twist cmd;

    if (push_state_ == INACTIVE)
        cmd = getNullTwist();

    else if (push_state_ == RELOCATE)
        cmd = relocateVelocities();

    else if (push_state_ == APPROACH)
        cmd = approachVelocities();

    else if (push_state_ == PUSH)
        cmd = getVelocities();

    //velocity limits

    if(fabs(cmd.linear.x) > vel_lin_max_) cmd.linear.x = (cmd.linear.x > 0 ? vel_lin_max_ : - vel_lin_max_);
    if(fabs(cmd.linear.y) > vel_lin_max_) cmd.linear.y = (cmd.linear.y > 0 ? vel_lin_max_ : - vel_lin_max_);
    if(fabs(cmd.angular.y) > vel_ang_max_) cmd.angular.y = (cmd.angular.y > 0 ? vel_ang_max_ : - vel_ang_max_);

    return cmd;
}

geometry_msgs::Twist PushPlanner::relocateVelocities(){
    geometry_msgs::Twist cmd = getNullTwist();

    double attraction_coefficient = 0.6;
    double repulsion_coefficient = 0.6;
    double repulsion_threshold = 2 * object_diameter_;
    double rotation_coefficient = 0.3;

    // Attraction
    double G_attr_x = -attraction_coefficient*(pose_robot_.x - relocate_target_(0));
    double G_attr_y = -attraction_coefficient*(pose_robot_.y - relocate_target_(1));

    // Repulsion
    double G_rep_x, G_rep_y;

    if (dR2O < repulsion_threshold){
        G_rep_x =  - repulsion_coefficient * (pose_robot_.x - pose_object_.pose.position.x) * (1 / pow(dR2O,2) - repulsion_threshold / pow(dR2O,3));
        G_rep_y =  - repulsion_coefficient * (pose_robot_.y - pose_object_.pose.position.y) * (1 / pow(dR2O,2) - repulsion_threshold / pow(dR2O,3));
    }
    else {
        G_rep_x = 0;
        G_rep_y = 0;
    }
    
    double G_x = G_attr_x + G_rep_x;
    double G_y = G_attr_y + G_rep_y;

    vec vel_R_  = rotate2DVector(G_x ,G_y, -pose_robot_.theta);
    cmd.linear.x = vel_R_(0);
    cmd.linear.y = vel_R_(1);

    //orientation cmd
    cmd.angular.z = - rotation_coefficient * rotationDifference(aR2O, pose_robot_.theta);

    return cmd;

}

geometry_msgs::Twist PushPlanner::approachVelocities(){
    geometry_msgs::Twist cmd = getNullTwist();

    double attraction_coefficient = 0.6;
    double rotation_coefficient = 0.3;

    // Attraction
    double G_attr_x = -attraction_coefficient*(pose_robot_.x - pose_object_.pose.position.x);
    double G_attr_y = -attraction_coefficient*(pose_robot_.y - pose_object_.pose.position.y);

    vec vel_R_  = rotate2DVector(G_attr_x ,G_attr_y, -pose_robot_.theta);
    cmd.linear.x = vel_R_(0);
    cmd.linear.y = vel_R_(1);

    //orientation cmd
    cmd.angular.z = - rotation_coefficient * rotationDifference(aO2P, pose_robot_.theta);

    cout<<"approach"<<endl<<cmd<<endl;
    return cmd;

}

void PushPlanner::updateMatrix(){
    current_target_vec_(elem_count_, 0) = current_target_.pose.position.x;
    current_target_vec_(elem_count_, 1) = current_target_.pose.position.y;
    current_target_vec_(elem_count_, 2) = tf::getYaw(current_target_.pose.orientation);

    pose_object_vec_(elem_count_, 0) = pose_object_.pose.position.x;
    pose_object_vec_(elem_count_, 1) = pose_object_.pose.position.y;
    pose_object_vec_(elem_count_, 2) = tf::getYaw(pose_object_.pose.orientation);

    pose_robot_vec_(elem_count_, 0) = pose_robot_.x;
    pose_robot_vec_(elem_count_, 1) = pose_robot_.y;
    pose_robot_vec_(elem_count_, 2) = pose_robot_.theta;

    elem_count_++;

    if (elem_count_ == current_target_vec_.n_rows) {
        current_target_vec_.resize(current_target_vec_.n_rows + pushing_path_.poses.size(), 3);
        pose_robot_vec_.resize(current_target_vec_.n_rows + pushing_path_.poses.size(), 3);
        pose_object_vec_.resize(current_target_vec_.n_rows + pushing_path_.poses.size(), 3);
    }

}


