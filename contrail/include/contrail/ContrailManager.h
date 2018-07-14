#pragma once

#include <ros/ros.h>
#include <dynamic_reconfigure/server.h>

#include <nav_msgs/Path.h>
#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/Pose.h>
#include <geometry_msgs/Point.h>
#include <geometry_msgs/Vector3.h>
#include <geometry_msgs/Quaternion.h>

#include <contrail_msgs/CubicSpline.h>
#include <contrail_msgs/SetTracking.h>
#include <contrail/ManagerParamsConfig.h>

#include <mavros_msgs/PositionTarget.h>

#include <eigen3/Eigen/Dense>

class ContrailManager {
	public:
		enum TrackingRef {
			NONE,
			SPLINE,
			PATH,
			POSE
		};

	private:
		ros::NodeHandle& nhp_;

		ros::Subscriber sub_spline_;
		ros::Subscriber sub_path_;
		ros::Subscriber sub_pose_;

		ros::Publisher pub_discrete_progress_;	//Publishes the current path index when a discrete setpoint is reached

		ros::ServiceServer srv_set_tracking_;
		dynamic_reconfigure::Server<contrail::ManagerParamsConfig> dyncfg_settings_;

		contrail_msgs::CubicSpline msg_spline_;
		nav_msgs::Path msg_path_;
		geometry_msgs::PoseStamped msg_pose_;

		bool param_fallback_to_pose_;
		bool pose_reached_;	//Check so that waypoint messages for pose are only output once
		int path_c_; //Counter for the current path step
		ros::Time dsp_reached_time_; //Check to keep track of if the current discrete setpoint has been reached already
		ros::Duration param_hold_duration_;
		double param_dsp_radius_;
		double param_dsp_yaw_;

		TrackingRef tracked_ref_;

	public:
		ContrailManager( ros::NodeHandle nh, const bool use_init_pose = false, const Eigen::Affine3d init_pose = Eigen::Affine3d::Identity() );

		~ContrailManager( void );

		//Allows control over the tracked reference
		TrackingRef get_reference_used( void );
		bool set_reference_used( TrackingRef state, const ros::Time t, const bool update_dsp_progress = false );

		//Returns the indicated reference can currently be tracked
		//has_reference() can be used to wait for an initial setpoint
		bool has_reference( const ros::Time t );
		bool has_spline_reference( const ros::Time t );
		bool has_path_reference( void );
		bool has_pose_reference( void );

		//Gets the current reference from the latest updated source
		//Returns true if the reference was successfully obtained
		bool get_reference( mavros_msgs::PositionTarget &ref, const ros::Time t, const geometry_msgs::Pose &p_c );

		//Gets the current reference from the spline source
		//Returns true if the reference was successfully obtained
		bool get_spline_reference( mavros_msgs::PositionTarget &ref, const ros::Time t );

		//Gets the current reference from the discrete path source
		//Returns true if the reference was successfully obtained
		bool get_discrete_path_reference( mavros_msgs::PositionTarget &ref, const ros::Time t, const geometry_msgs::Pose &p_c );

		//Gets the current reference from the discrete pose source
		//Returns true if the reference was successfully obtained
		bool get_discrete_pose_reference( mavros_msgs::PositionTarget &ref, const ros::Time t, const geometry_msgs::Pose &p_c );

		//Convinience versions of the get*reference();
		bool get_reference( mavros_msgs::PositionTarget &ref, const ros::Time t, const Eigen::Affine3d &g_c );
		bool get_discrete_path_reference( mavros_msgs::PositionTarget &ref, const ros::Time t, const Eigen::Affine3d &g_c );
		bool get_discrete_pose_reference( mavros_msgs::PositionTarget &ref, const ros::Time t, const Eigen::Affine3d &g_c );

		//Allows the trackings to be set directly rather than by callback
		bool set_spline_reference( const contrail_msgs::CubicSpline& spline );
		bool set_discrete_path_reference( const nav_msgs::Path& path );
		bool set_discrete_pose_reference( const geometry_msgs::PoseStamped& pose, const bool is_fallback = false );

	private:
		//ROS callbacks
		void callback_cfg_settings( contrail::ManagerParamsConfig &config, uint32_t level );

		void callback_spline( const contrail_msgs::CubicSpline::ConstPtr& msg_in );
		void callback_discrete_path( const nav_msgs::Path::ConstPtr& msg_in );
		void callback_discrete_pose( const geometry_msgs::PoseStamped::ConstPtr& msg_in );
		bool callback_set_tracking( contrail_msgs::SetTracking::Request &req, contrail_msgs::SetTracking::Response &res );

		void publish_waypoint_reached( const std::string frame_id, const ros::Time t, const uint32_t wp_c, const uint32_t wp_num );

		//Returns true if the messages contain valid data
		bool check_msg_spline(const contrail_msgs::CubicSpline& spline, const ros::Time t );
		bool check_msg_path(const nav_msgs::Path& path );
		bool check_msg_pose(const geometry_msgs::PoseStamped& pose );

		//Returns true of the tracking point has been reached
		bool check_waypoint_reached( const Eigen::Vector3d& pos_s, const double yaw_s, const Eigen::Vector3d& pos_c, const double yaw_c );
		bool check_waypoint_complete( const ros::Time t );
		void reset_waypoint_timer( void );

		//Calculates the radial distance between two points
		double radial_dist( const Eigen::Vector3d a, const Eigen::Vector3d b );
		double rotation_dist( const double a, const double b );

		//Convinence functions for generating a position target from a pose
		mavros_msgs::PositionTarget target_from_pose( const std::string& frame_id, const geometry_msgs::Pose& p );
		mavros_msgs::PositionTarget target_from_pose( const std::string& frame_id, const Eigen::Affine3d& g );

		double yaw_from_quaternion( const geometry_msgs::Quaternion &q );
		double yaw_from_quaternion( const Eigen::Quaterniond &q );

		Eigen::Vector3d position_from_msg( const geometry_msgs::Point &p );
		Eigen::Quaterniond quaternion_from_msg( const geometry_msgs::Quaternion &q );
		Eigen::Affine3d affine_from_msg( const geometry_msgs::Pose &pose );

		geometry_msgs::Vector3 vector_from_eig( const Eigen::Vector3d &v );
		geometry_msgs::Point point_from_eig( const Eigen::Vector3d &p );
		geometry_msgs::Quaternion quaternion_from_eig( const Eigen::Quaterniond &q );
		geometry_msgs::Pose pose_from_eig( const Eigen::Affine3d &g );

};
