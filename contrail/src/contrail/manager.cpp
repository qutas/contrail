#include <ros/ros.h>

#include <contrail/ContrailManager.h>

#include <nav_msgs/Path.h>
#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/Pose.h>
#include <geometry_msgs/Point.h>
#include <geometry_msgs/Vector3.h>
#include <geometry_msgs/Quaternion.h>

#include <contrail_msgs/CubicSpline.h>
#include <contrail_msgs/WaypointProgress.h>
#include <contrail_msgs/SetTracking.h>
#include <contrail/ManagerParamsConfig.h>

#include <mavros_msgs/PositionTarget.h>

#include <eigen3/Eigen/Dense>
#include <string>
#include <math.h>


//=======================
// Public
//=======================

ContrailManager::ContrailManager( ros::NodeHandle nh, const bool use_init_pose, const Eigen::Affine3d init_pose ) :
	nhp_(nh),
	tracked_ref_(TrackingRef::NONE),
	param_fallback_to_pose_(false),
	pose_reached_(false),
	path_c_(0),
	dsp_reached_time_(0),
	param_hold_duration_(0),
	param_dsp_radius_(0.0),
	param_dsp_yaw_(0.0),
	dyncfg_settings_(ros::NodeHandle(nh, "contrail")) {

	dyncfg_settings_.setCallback(boost::bind(&ContrailManager::callback_cfg_settings, this, _1, _2));

	sub_spline_ = nhp_.subscribe<contrail_msgs::CubicSpline>( "reference/contrail/spline", 10, &ContrailManager::callback_spline, this );
	sub_path_ = nhp_.subscribe<nav_msgs::Path>( "reference/contrail/path", 10, &ContrailManager::callback_discrete_path, this );
	sub_pose_ = nhp_.subscribe<geometry_msgs::PoseStamped>( "reference/contrail/pose", 10, &ContrailManager::callback_discrete_pose, this );

	pub_discrete_progress_ = nhp_.advertise<contrail_msgs::WaypointProgress>( "feedback/contrail/discrete_progress", 10 );

	srv_set_tracking_ = nhp_.advertiseService("contrail/set_tracking", &ContrailManager::callback_set_tracking, this);

	//Apply the initial pose setpoint if desired
	if(use_init_pose) {
		geometry_msgs::PoseStamped temp_pose;
		 //XXX: Just set a fake time for the init pose so it registers with has_pose_reference()
		temp_pose.header.stamp = ros::Time(1);
		temp_pose.pose = pose_from_eig(init_pose);
		set_discrete_pose_reference(temp_pose);
	}
}

ContrailManager::~ContrailManager( void ) {

}

ContrailManager::TrackingRef ContrailManager::get_reference_used( void ) {
	return tracked_ref_;
}

bool ContrailManager::set_reference_used( ContrailManager::TrackingRef state, const ros::Time t, bool update_dsp_progress ) {
	bool success = false;

	if(tracked_ref_ != state) {
		if( state == TrackingRef::NONE ) {
			success = true;
		} else if( ( state == TrackingRef::SPLINE ) && has_spline_reference(t) ) {
			ROS_INFO("[Contrail] Setting tracking to spline reference");
			success = true;
		} else if( ( state == TrackingRef::PATH ) && has_path_reference() ) {
			ROS_INFO("[Contrail] Setting tracking to path reference");
			update_dsp_progress = true;	//Always update discrete progress if switching

			success = true;
		} else if( ( state == TrackingRef::POSE ) && has_pose_reference() ) {
			ROS_INFO("[Contrail] Setting tracking to pose reference");
			if(!pose_reached_)
				update_dsp_progress = true;
			success = true;
		}

		if(success) {
			tracked_ref_ = state;

			//If we switched out of pose ref, then reset it
			if(tracked_ref_ != TrackingRef::POSE)
				pose_reached_ = false;
		}
	} else {
		//No need for a tracking change
		//But accept anyway
		success = true;
	}

	if(update_dsp_progress) {
		//Edge case for updating dsp output
		if(state == TrackingRef::PATH) {
			publish_waypoint_reached(msg_path_.header.frame_id, t, path_c_, msg_path_.poses.size());
		} else if(state == TrackingRef::POSE) {
			publish_waypoint_reached(msg_pose_.header.frame_id, t, 0, 1);
		}
	}

	return success;
}

bool ContrailManager::has_reference( const ros::Time t ) {
	return has_spline_reference(t) || has_path_reference() || has_pose_reference();
}

bool ContrailManager::has_spline_reference( const ros::Time t ) {
	return check_msg_spline(msg_spline_, t);
}

bool ContrailManager::has_path_reference( void ) {
	return check_msg_path(msg_path_);
}

bool ContrailManager::has_pose_reference( void ) {
	return check_msg_pose(msg_pose_);
}

bool ContrailManager::get_reference( mavros_msgs::PositionTarget &ref, const ros::Time t, const Eigen::Affine3d &g_c ) {
	bool success = false;

	if(get_reference_used() == TrackingRef::SPLINE) {
		success = get_spline_reference(ref, t);
	} else if(get_reference_used() == TrackingRef::PATH) {
		success = get_discrete_path_reference(ref, t, g_c);
	} else if(get_reference_used() == TrackingRef::POSE) {
		success = get_discrete_pose_reference(ref, t, g_c);
	}

	return success;
}

bool ContrailManager::get_spline_reference( mavros_msgs::PositionTarget &ref, const ros::Time t ) {
	bool success = true;

	if( has_spline_reference(t) ) {
		//TODO: Spline tracking
	}

	return success;
}

bool ContrailManager::get_discrete_path_reference( mavros_msgs::PositionTarget &ref, const ros::Time t, const Eigen::Affine3d &g_c ) {
	bool success = false;

	if( has_path_reference() ) {
		//Prepare the tracked pose
		geometry_msgs::PoseStamped ps;
		ps.header.frame_id = msg_path_.header.frame_id;
		ps.header.stamp = t;

		//Keep tracking the poses as long as we haven't reached the end unexpectedly
		if(path_c_ < msg_path_.poses.size()) {
			//Just track the last pose
			ps.pose = msg_path_.poses[path_c_].pose;
			success = true;

			//Check all the logic for the next loop
			if( check_waypoint_reached(position_from_msg(msg_path_.poses[path_c_].pose.position),
									   yaw_from_quaternion(quaternion_from_msg(msg_path_.poses[path_c_].pose.orientation)),
									   g_c.translation(),
									   yaw_from_quaternion(Eigen::Quaterniond(g_c.linear()))) ) {
				//If reached the goal
				if( check_waypoint_complete(t) ) {
					if( path_c_ < msg_path_.poses.size() ) {
						//There are more waypoints left in the path
						path_c_++;
						reset_waypoint_timer();
					}

					//Update the path status
					publish_waypoint_reached(msg_path_.header.frame_id, t, path_c_, msg_path_.poses.size());

					//Handle the path being complete
					if( path_c_ >= msg_path_.poses.size() ) {
						if(param_fallback_to_pose_) {
							//Just track the last pose
							geometry_msgs::PoseStamped hold;
							hold.header = msg_path_.header;
							hold.pose = msg_path_.poses[msg_path_.poses.size()-1].pose;

							//Should never fail, but just in case
							if(!set_discrete_pose_reference(hold, true) ) {
								ROS_ERROR("Error changing fallback tracking reference!");
							}
						} else {
							//Set back to no tracking
							if(!set_reference_used(TrackingRef::NONE, t)) {
								ROS_ERROR("Error changing tracking reference!");
							}
						}

						ROS_INFO("Finished path reference");

						//Reset the path tracking
						path_c_ = 0;
						reset_waypoint_timer();
					}
				} //else we haven't stayed long enough, so keep tracking as usual
			} else {
				//We're outside the waypoint, so keep tracking
				reset_waypoint_timer();
			}
		}

		if(success) {
			ref = target_from_pose(msg_path_.header.frame_id, ps.pose);
		}
	}

	return success;
}

bool ContrailManager::get_discrete_pose_reference( mavros_msgs::PositionTarget &ref, const ros::Time t, const Eigen::Affine3d &g_c ) {
	bool success = true;

	if( has_pose_reference() ) {
		//Should we do waypoint checking?
		if(!pose_reached_) {
			//There is a valid pose reference
			if( check_waypoint_reached(position_from_msg(msg_pose_.pose.position),
									   yaw_from_quaternion(quaternion_from_msg(msg_pose_.pose.orientation)),
									   g_c.translation(),
									   yaw_from_quaternion(Eigen::Quaterniond(g_c.linear()))) ) {
				//If reached the goal
				if( check_waypoint_complete(t) ) {
					//Only send out the reached waypoint if reverting back to no tracking
					publish_waypoint_reached(msg_pose_.header.frame_id, t, 1, 1);

					if(!param_fallback_to_pose_) {
						//The waypoint is over, so reset to no tracking
						set_reference_used(TrackingRef::NONE, t);
					}

					ROS_INFO("Reached pose reference");

					//Set a flag to not bother checking waypoint later on
					pose_reached_ = true;
					reset_waypoint_timer();
				}
			} else {
				//We're outside the waypoint, so keep tracking
				reset_waypoint_timer();
			}
		}

		ref = target_from_pose(msg_pose_.header.frame_id, msg_pose_.pose);
		success = true;
	}

	return success;
}

bool ContrailManager::get_reference( mavros_msgs::PositionTarget &ref, const ros::Time t, const geometry_msgs::Pose &p_c ) {
	return get_reference(ref, t, affine_from_msg(p_c));
}

bool ContrailManager::get_discrete_path_reference( mavros_msgs::PositionTarget &ref, const ros::Time t, const geometry_msgs::Pose &p_c ) {
	return get_discrete_path_reference(ref, t, affine_from_msg(p_c));
}

bool ContrailManager::get_discrete_pose_reference( mavros_msgs::PositionTarget &ref, const ros::Time t, const geometry_msgs::Pose &p_c ) {
	return get_discrete_pose_reference(ref, t, affine_from_msg(p_c));
}

bool ContrailManager::set_spline_reference( const contrail_msgs::CubicSpline& spline ) {
	bool success = false;
	ros::Time tc = ros::Time::now();

	if( check_msg_spline(spline, tc) ) {
		msg_spline_ = spline;

		if(!set_reference_used(TrackingRef::SPLINE, tc)) {
			ROS_ERROR("Error changing tracking reference!");
		}

		success = true;
	}

	return success;
}

bool ContrailManager::set_discrete_path_reference( const nav_msgs::Path& path ) {
	bool success = false;
	ros::Time tc = ros::Time::now();

	if( check_msg_path(path) ) {
		msg_path_ = path;
		path_c_ = 0;
		reset_waypoint_timer();

		//Request a reference change, and ensure discrete progress update
		if(!set_reference_used(TrackingRef::PATH, tc, true)) {
			ROS_ERROR("Error changing tracking reference!");
		}

		success = true;
	}

	return success;
}

bool ContrailManager::set_discrete_pose_reference( const geometry_msgs::PoseStamped& pose, const bool is_fallback ) {
	bool success = false;
	ros::Time tc = ros::Time::now();

	if( check_msg_pose(pose) ) {
		msg_pose_ = pose;
		reset_waypoint_timer();
		pose_reached_ = is_fallback;

		//Request a reference change, and update discrete progress as long as it's not a fallback switch
		if(!set_reference_used(TrackingRef::POSE, tc, !is_fallback)) {
			ROS_ERROR("Error changing tracking reference!");
		}

		success = true;
	}

	return success;
}


//=======================
// Private
//=======================

void ContrailManager::callback_cfg_settings( contrail::ManagerParamsConfig &config, uint32_t level ) {
	param_fallback_to_pose_ = config.fallback_to_pose;
	param_hold_duration_ = ros::Duration(config.waypoint_hold_duration);
	param_dsp_radius_ = config.waypoint_radius;
	param_dsp_yaw_ = config.waypoint_yaw_accuracy;
}

void ContrailManager::callback_spline( const contrail_msgs::CubicSpline::ConstPtr& msg_in ) {
	if(!set_spline_reference(*msg_in)) {
		ROS_WARN("[Contrail] Spline reference invalid, ignoring");
	}
}

void ContrailManager::callback_discrete_path( const nav_msgs::Path::ConstPtr& msg_in ) {
	if(!set_discrete_path_reference(*msg_in)) {
		ROS_WARN("[Contrail] Path reference invalid, ignoring");
	}
}

void ContrailManager::callback_discrete_pose( const geometry_msgs::PoseStamped::ConstPtr& msg_in ) {
	if(!set_discrete_pose_reference(*msg_in)) {
		ROS_WARN("[Contrail] Pose reference invalid, ignoring");
	}
}

void ContrailManager::publish_waypoint_reached( const std::string frame_id, const ros::Time t, const uint32_t wp_c, const uint32_t wp_num ) {
	contrail_msgs::WaypointProgress msg_out;

	msg_out.header.frame_id = frame_id;
	msg_out.header.stamp = t;

	msg_out.current = wp_c;
	msg_out.progress = ((double)wp_c) / wp_num;

	pub_discrete_progress_.publish(msg_out);
}

bool ContrailManager::callback_set_tracking( contrail_msgs::SetTracking::Request  &req,
											 contrail_msgs::SetTracking::Response &res ) {
	if( req.tracking == req.TRACKING_NONE ) {
		res.success = set_reference_used(TrackingRef::NONE, ros::Time::now());
	} else if( req.tracking == req.TRACKING_SPLINE ) {
		res.success = set_reference_used(TrackingRef::SPLINE, ros::Time::now());
	} else if( req.tracking == req.TRACKING_PATH ) {
		res.success = set_reference_used(TrackingRef::PATH, ros::Time::now());
	} else if( req.tracking == req.TRACKING_POSE ) {
		res.success = set_reference_used(TrackingRef::POSE, ros::Time::now());
	} else {
		res.success = false;
	}

	return true;
}

bool ContrailManager::check_msg_spline(const contrail_msgs::CubicSpline& spline, const ros::Time t ) {
	//TODO: Check spline time is recent/not finished
	bool tvec_check = false;
	bool xvec_check = false;
	bool yvec_check = false;
	bool zvec_check = false;
	bool rvec_check = false;

	if(spline.header.stamp > ros::Time(0)) {
		tvec_check = (spline.t.size() > 0);
		xvec_check = (spline.x.size() == spline.t.size());
		yvec_check = (spline.y.size() == spline.t.size());
		zvec_check = (spline.z.size() == spline.t.size());
		rvec_check = (spline.yaw.size() == spline.t.size());
	}

	return (tvec_check && xvec_check &&  yvec_check &&  zvec_check && rvec_check);
}

bool ContrailManager::check_msg_path(const nav_msgs::Path& path ) {
	return ( (path.header.stamp > ros::Time(0)) && (path.poses.size() > 0) );
}

bool ContrailManager::check_msg_pose(const geometry_msgs::PoseStamped& pose ) {
	return pose.header.stamp > ros::Time(0);
}

bool ContrailManager::check_waypoint_reached( const Eigen::Vector3d& pos_s, const double yaw_s, const Eigen::Vector3d& pos_c, const double yaw_c ) {
	return ( (radial_dist(pos_s, pos_c) < param_dsp_radius_) && (rotation_dist(yaw_s, yaw_c) < param_dsp_yaw_) );
}

bool ContrailManager::check_waypoint_complete( const ros::Time t ) {
	bool reached = false;

	if( dsp_reached_time_ == ros::Time(0) ) {
		//if the waypoint was only just reached
		dsp_reached_time_ = t;
	} else if( (t - dsp_reached_time_) > param_hold_duration_) {
		//we had previously reached the waypoint, and we've been there long enough
		reached = true;
	}

	return reached;
}

void ContrailManager::reset_waypoint_timer( void ) {
	dsp_reached_time_ = ros::Time(0);
}

double ContrailManager::radial_dist( const Eigen::Vector3d a, const Eigen::Vector3d b ) {
	return (a - b).norm();
}

double ContrailManager::rotation_dist( const double a, const double b ) {
	double rad = std::fabs(a - b);
	//Adjust for +- short rotation
	return (rad > M_PI) ? rad - M_PI : rad;
}

mavros_msgs::PositionTarget ContrailManager::target_from_pose( const std::string& frame_id, const geometry_msgs::Pose &p ) {
	mavros_msgs::PositionTarget msg_out;

	msg_out.header.stamp = ros::Time::now();
	msg_out.header.frame_id = frame_id;

	msg_out.coordinate_frame = msg_out.FRAME_LOCAL_NED;
	msg_out.type_mask = msg_out.IGNORE_VX | msg_out.IGNORE_VY | msg_out.IGNORE_VZ |
						msg_out.IGNORE_AFX | msg_out.IGNORE_AFY | msg_out.IGNORE_AFZ |
						msg_out.FORCE | msg_out.IGNORE_YAW_RATE;

	msg_out.position = p.position;
	msg_out.yaw = yaw_from_quaternion(quaternion_from_msg(p.orientation));

	msg_out.velocity.x = 0.0;
	msg_out.velocity.y = 0.0;
	msg_out.velocity.z = 0.0;
	msg_out.acceleration_or_force.x = 0.0;
	msg_out.acceleration_or_force.y = 0.0;
	msg_out.acceleration_or_force.z = 0.0;
	msg_out.yaw_rate = 0.0;

	return msg_out;
}

mavros_msgs::PositionTarget ContrailManager::target_from_pose( const std::string& frame_id, const Eigen::Affine3d &g ) {
	return target_from_pose( frame_id, pose_from_eig(g) );
}

double ContrailManager::yaw_from_quaternion( const geometry_msgs::Quaternion &q ) {
	return yaw_from_quaternion(quaternion_from_msg(q));
}

double ContrailManager::yaw_from_quaternion( const Eigen::Quaterniond &q ) {
	double siny = +2.0 * (q.w() * q.z() + q.x() * q.y());
	double cosy = +1.0 - 2.0 * (q.y() * q.y() + q.z() * q.z());
	return std::atan2(siny, cosy);
}

Eigen::Vector3d ContrailManager::position_from_msg(const geometry_msgs::Point &p) {
	return Eigen::Vector3d(p.x, p.y, p.z);
}

Eigen::Quaterniond ContrailManager::quaternion_from_msg(const geometry_msgs::Quaternion &q) {
	return Eigen::Quaterniond(q.w, q.x, q.y, q.z).normalized();
}

Eigen::Affine3d ContrailManager::affine_from_msg(const geometry_msgs::Pose &pose) {
	Eigen::Affine3d a;

	a.translation() << position_from_msg(pose.position);
	a.linear() << quaternion_from_msg(pose.orientation).toRotationMatrix();

	return a;
}

geometry_msgs::Vector3 ContrailManager::vector_from_eig(const Eigen::Vector3d &v) {
	geometry_msgs::Vector3 vec;

	vec.x = v.x();
	vec.y = v.y();
	vec.z = v.z();

	return vec;
}

geometry_msgs::Point ContrailManager::point_from_eig(const Eigen::Vector3d &p) {
	geometry_msgs::Point point;

	point.x = p.x();
	point.y = p.y();
	point.z = p.z();

	return point;
}

geometry_msgs::Quaternion ContrailManager::quaternion_from_eig(const Eigen::Quaterniond &q) {
	geometry_msgs::Quaternion quat;
	Eigen::Quaterniond qn = q.normalized();

	quat.w = qn.w();
	quat.x = qn.x();
	quat.y = qn.y();
	quat.z = qn.z();

	return quat;
}

geometry_msgs::Pose ContrailManager::pose_from_eig(const Eigen::Affine3d &g) {
	geometry_msgs::Pose pose;

	pose.position = point_from_eig(g.translation());
	pose.orientation = quaternion_from_eig(Eigen::Quaterniond(g.linear()));

	return pose;
}

