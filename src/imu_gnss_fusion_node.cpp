#include <eigen_conversions/eigen_msg.h>
#include <nav_msgs/Odometry.h>
#include <nav_msgs/Path.h>
#include <ros/ros.h>
#include <sensor_msgs/Imu.h>
#include <sensor_msgs/NavSatFix.h>

#include <deque>
#include <fstream>
#include <iostream>

#include "imu_x_fusion/gnss.hpp"
#include "imu_x_fusion/kf.hpp"

namespace cg {

class FusionNode {
 public:
  FusionNode(ros::NodeHandle &nh) {
    double acc_n, gyr_n, acc_w, gyr_w;
    nh.param("acc_noise", acc_n, 1e-2);
    nh.param("gyr_noise", gyr_n, 1e-4);
    nh.param("acc_bias_noise", acc_w, 1e-6);
    nh.param("gyr_bias_noise", gyr_w, 1e-8);

    kf_ptr_ = std::make_unique<KF>(acc_n, gyr_n, acc_w, gyr_w);

    const double sigma_pv = 10;
    const double sigma_rp = 10 * kDegreeToRadian;
    const double sigma_yaw = 100 * kDegreeToRadian;
    kf_ptr_->set_cov(sigma_pv, sigma_pv, sigma_rp, sigma_yaw, 0.02, 0.02);

    std::string topic_imu = "/imu/data";
    std::string topic_gps = "/fix";

    imu_sub_ = nh.subscribe<sensor_msgs::Imu>(topic_imu, 10, boost::bind(&KF::imu_callback, kf_ptr_.get(), _1));
    gps_sub_ = nh.subscribe(topic_gps, 10, &FusionNode::gps_callback, this);

    path_pub_ = nh.advertise<nav_msgs::Path>("nav_path", 10);
    odom_pub_ = nh.advertise<nav_msgs::Odometry>("nav_odom", 10);

    // log files
    file_gps_.open("fusion_gps.csv");
    file_state_.open("fusion_state.csv");
  }

  ~FusionNode() {
    if (file_gps_.is_open()) file_gps_.close();
    if (file_state_.is_open()) file_state_.close();
  }

  void gps_callback(const sensor_msgs::NavSatFixConstPtr &gps_msg);

  void publish_save_state();

 private:
  ros::Subscriber imu_sub_;
  ros::Subscriber gps_sub_;

  ros::Publisher path_pub_;
  ros::Publisher odom_pub_;

  nav_msgs::Path nav_path_;

  Eigen::Vector3d init_lla_;
  Eigen::Vector3d I_p_Gps_ = Eigen::Vector3d(0., 0., 0.);

  KFPtr kf_ptr_;

  std::ofstream file_gps_;
  std::ofstream file_state_;
};

void FusionNode::gps_callback(const sensor_msgs::NavSatFixConstPtr &gps_msg) {
  if (gps_msg->status.status != 2) {
    printf("[cggos %s] ERROR: Bad GPS Message!!!\n", __FUNCTION__);
    return;
  }

  GpsDataPtr gps_data_ptr = std::make_shared<GpsData>();
  gps_data_ptr->timestamp = gps_msg->header.stamp.toSec();
  gps_data_ptr->lla[0] = gps_msg->latitude;
  gps_data_ptr->lla[1] = gps_msg->longitude;
  gps_data_ptr->lla[2] = gps_msg->altitude;
  gps_data_ptr->cov = Eigen::Map<const Eigen::Matrix3d>(gps_msg->position_covariance.data());

  if (!kf_ptr_->initialized_) {
    if (kf_ptr_->imu_buf_.size() < kf_ptr_->kImuBufSize) {
      printf("[cggos %s] ERROR: Not Enough IMU data for Initialization!!!\n", __FUNCTION__);
      return;
    }

    kf_ptr_->last_imu_ptr_ = kf_ptr_->imu_buf_.back();
    if (std::abs(gps_data_ptr->timestamp - kf_ptr_->last_imu_ptr_->timestamp) > 0.5) {
      printf("[cggos %s] ERROR: Gps and Imu timestamps are not synchronized!!!\n", __FUNCTION__);
      return;
    }

    kf_ptr_->state_ptr_->timestamp = kf_ptr_->last_imu_ptr_->timestamp;

    if (!kf_ptr_->init_rot_from_imudata(kf_ptr_->state_ptr_->r_GI, kf_ptr_->imu_buf_)) return;

    init_lla_ = gps_data_ptr->lla;

    printf("[cggos %s] System initialized.\n", __FUNCTION__);

    return;
  }

  // convert WGS84 to ENU frame
  Eigen::Vector3d p_G_Gps;
  cg::lla2enu(init_lla_, gps_data_ptr->lla, &p_G_Gps);

  const Eigen::Vector3d &p_GI = kf_ptr_->state_ptr_->p_GI;
  const Eigen::Matrix3d &r_GI = kf_ptr_->state_ptr_->r_GI;

  // residual
  Eigen::Vector3d residual = p_G_Gps - (p_GI + r_GI * I_p_Gps_);

  // jacobian
  Eigen::Matrix<double, 3, kStateDim> H;
  H.setZero();
  H.block<3, 3>(0, 0) = Eigen::Matrix3d::Identity();
  H.block<3, 3>(0, 6) = -r_GI * cg::skew_matrix(I_p_Gps_);

  // measurement covariance
  const Eigen::Matrix3d &R = gps_data_ptr->cov;

  Eigen::Matrix<double, kStateDim, 3> K;
  kf_ptr_->update_K(H, R, K);
  kf_ptr_->update_P(H, R, K);
  *kf_ptr_->state_ptr_ = *kf_ptr_->state_ptr_ + K * residual;

  // save data
  {
    publish_save_state();

    file_gps_ << std::fixed << std::setprecision(15) << gps_data_ptr->timestamp << ", " << gps_data_ptr->lla[0] << ", "
              << gps_data_ptr->lla[1] << ", " << gps_data_ptr->lla[2] << std::endl;
  }
}

void FusionNode::publish_save_state() {
  // publish the odometry
  std::string fixed_id = "global";
  nav_msgs::Odometry odom_msg;
  odom_msg.header.frame_id = fixed_id;
  odom_msg.header.stamp = ros::Time::now();
  Eigen::Isometry3d T_wb = Eigen::Isometry3d::Identity();
  T_wb.linear() = kf_ptr_->state_ptr_->r_GI;
  T_wb.translation() = kf_ptr_->state_ptr_->p_GI;
  tf::poseEigenToMsg(T_wb, odom_msg.pose.pose);
  tf::vectorEigenToMsg(kf_ptr_->state_ptr_->v_GI, odom_msg.twist.twist.linear);
  Eigen::Matrix3d P_pp = kf_ptr_->state_ptr_->cov.block<3, 3>(0, 0);
  Eigen::Matrix3d P_po = kf_ptr_->state_ptr_->cov.block<3, 3>(0, 6);
  Eigen::Matrix3d P_op = kf_ptr_->state_ptr_->cov.block<3, 3>(6, 0);
  Eigen::Matrix3d P_oo = kf_ptr_->state_ptr_->cov.block<3, 3>(6, 6);
  Eigen::Matrix<double, 6, 6, Eigen::RowMajor> P_imu_pose = Eigen::Matrix<double, 6, 6>::Zero();
  P_imu_pose << P_pp, P_po, P_op, P_oo;
  for (int i = 0; i < 36; i++) odom_msg.pose.covariance[i] = P_imu_pose.data()[i];
  odom_pub_.publish(odom_msg);

  // publish the path
  geometry_msgs::PoseStamped pose_stamped;
  pose_stamped.header = odom_msg.header;
  pose_stamped.pose = odom_msg.pose.pose;
  nav_path_.header = pose_stamped.header;
  nav_path_.poses.push_back(pose_stamped);
  path_pub_.publish(nav_path_);

  // save state p q lla
  std::shared_ptr<State> kf_state(kf_ptr_->state_ptr_);
  Eigen::Vector3d lla;
  cg::enu2lla(init_lla_, kf_state->p_GI, &lla);  // convert ENU state to lla
  const Eigen::Quaterniond q_GI(kf_state->r_GI);
  file_state_ << std::fixed << std::setprecision(15) << kf_state->timestamp << ", " << kf_state->p_GI[0] << ", "
              << kf_state->p_GI[1] << ", " << kf_state->p_GI[2] << ", " << q_GI.x() << ", " << q_GI.y() << ", "
              << q_GI.z() << ", " << q_GI.w() << ", " << lla[0] << ", " << lla[1] << ", " << lla[2] << std::endl;
}

}  // namespace cg

int main(int argc, char **argv) {
  ros::init(argc, argv, "imu_gnss_fusion");

  ros::NodeHandle nh;
  cg::FusionNode fusion_node(nh);

  ros::spin();

  return 0;
}
