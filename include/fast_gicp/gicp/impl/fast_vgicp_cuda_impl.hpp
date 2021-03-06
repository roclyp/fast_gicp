#ifndef FAST_GICP_FAST_VGICP_CUDA_IMPL_HPP
#define FAST_GICP_FAST_VGICP_CUDA_IMPL_HPP

#include <atomic>
#include <Eigen/Core>
#include <Eigen/Geometry>

#include <pcl/point_types.h>
#include <pcl/point_cloud.h>
#include <pcl/search/kdtree.h>
#include <pcl/registration/registration.h>

#include <fast_gicp/gicp/fast_vgicp_cuda.hpp>
#include <fast_gicp/cuda/fast_vgicp_cuda.cuh>

#include <thrust/host_vector.h>
#include <thrust/device_vector.h>

namespace fast_gicp {

template<typename PointSource, typename PointTarget>
FastVGICPCuda<PointSource, PointTarget>::FastVGICPCuda() : FastVGICP<PointSource, PointTarget>() {
  this->reg_name_ = "FastVGICPCuda";

  neighbor_search_method_ = NearestNeighborMethod::CPU_PARALLEL_KDTREE;

  vgicp_cuda.reset(new FastVGICPCudaCore());
  vgicp_cuda->set_max_iterations(this->max_iterations_);
  vgicp_cuda->set_rotation_epsilon(this->rotation_epsilon_);
  vgicp_cuda->set_transformation_epsilon(this->transformation_epsilon_);
  vgicp_cuda->set_resolution(this->voxel_resolution_);
}

template<typename PointSource, typename PointTarget>
FastVGICPCuda<PointSource, PointTarget>::~FastVGICPCuda() {}

template<typename PointSource, typename PointTarget>
void FastVGICPCuda<PointSource, PointTarget>::setNearesetNeighborSearchMethod(NearestNeighborMethod method) {
  neighbor_search_method_ = method;
}

template<typename PointSource, typename PointTarget>
void FastVGICPCuda<PointSource, PointTarget>::swapSourceAndTarget() {
  vgicp_cuda->swap_source_and_target();
  input_.swap(target_);
}

template<typename PointSource, typename PointTarget>
void FastVGICPCuda<PointSource, PointTarget>::clearSource() {
  input_.reset();
}

template<typename PointSource, typename PointTarget>
void FastVGICPCuda<PointSource, PointTarget>::clearTarget() {
  target_.reset();
}

template<typename PointSource, typename PointTarget>
void FastVGICPCuda<PointSource, PointTarget>::setInputSource(const PointCloudSourceConstPtr& cloud) {
  // the input cloud is the same as the previous one
  if(cloud == input_) {
    return;
  }

  pcl::Registration<PointSource, PointTarget, Scalar>::setInputSource(cloud);

  std::vector<Eigen::Vector3f, Eigen::aligned_allocator<Eigen::Vector3f>> points(cloud->size());
  std::transform(cloud->begin(), cloud->end(), points.begin(), [=](const PointSource& pt) { return pt.getVector3fMap(); });

  vgicp_cuda->set_source_cloud(points);
  switch(neighbor_search_method_) {
    case NearestNeighborMethod::CPU_PARALLEL_KDTREE: {
      std::vector<int> neighbors = find_neighbors_parallel_kdtree(k_correspondences_, cloud, source_kdtree);
      vgicp_cuda->set_source_neighbors(k_correspondences_, neighbors);
    } break;
    case NearestNeighborMethod::GPU_BRUTEFORCE:
      vgicp_cuda->find_source_neighbors(k_correspondences_);
      break;
  }
  vgicp_cuda->calculate_source_covariances(regularization_method_);

  std::vector<Eigen::Matrix3f, Eigen::aligned_allocator<Eigen::Matrix3f>> covs;
  vgicp_cuda->get_source_covariances(covs);
}

template<typename PointSource, typename PointTarget>
void FastVGICPCuda<PointSource, PointTarget>::setInputTarget(const PointCloudTargetConstPtr& cloud) {
  // the input cloud is the same as the previous one
  if(cloud == target_) {
    return;
  }

  pcl::Registration<PointSource, PointTarget, Scalar>::setInputTarget(cloud);

  std::vector<Eigen::Vector3f, Eigen::aligned_allocator<Eigen::Vector3f>> points(cloud->size());
  std::transform(cloud->begin(), cloud->end(), points.begin(), [=](const PointTarget& pt) { return pt.getVector3fMap(); });

  vgicp_cuda->set_target_cloud(points);
  switch(neighbor_search_method_) {
    case NearestNeighborMethod::CPU_PARALLEL_KDTREE: {
      std::vector<int> neighbors = find_neighbors_parallel_kdtree(k_correspondences_, cloud, target_kdtree);
      vgicp_cuda->set_target_neighbors(k_correspondences_, neighbors);
    } break;
    case NearestNeighborMethod::GPU_BRUTEFORCE:
      vgicp_cuda->find_target_neighbors(k_correspondences_);
      break;
  }
  vgicp_cuda->calculate_target_covariances(regularization_method_);
  vgicp_cuda->create_target_voxelmap();
}

template<typename PointSource, typename PointTarget>
void FastVGICPCuda<PointSource, PointTarget>::computeTransformation(PointCloudSource& output, const Matrix4& guess) {
  vgicp_cuda->set_max_iterations(this->max_iterations_);
  vgicp_cuda->set_rotation_epsilon(this->rotation_epsilon_);
  vgicp_cuda->set_transformation_epsilon(this->transformation_epsilon_);
  vgicp_cuda->set_resolution(this->voxel_resolution_);

  FastGICP<PointSource, PointTarget>::computeTransformation(output, guess);
}

template<typename PointSource, typename PointTarget>
template<typename PointT>
std::vector<int> FastVGICPCuda<PointSource, PointTarget>::find_neighbors_parallel_kdtree(int k, const boost::shared_ptr<const pcl::PointCloud<PointT>>& cloud, pcl::search::KdTree<PointT>& kdtree) const {
  kdtree.setInputCloud(cloud);
  std::vector<int> neighbors(cloud->size() * k);

#pragma omp parallel for
  for(int i = 0; i < cloud->size(); i++) {
    std::vector<int> k_indices;
    std::vector<float> k_sq_distances;
    kdtree.nearestKSearch(cloud->at(i), k, k_indices, k_sq_distances);

    std::copy(k_indices.begin(), k_indices.end(), neighbors.begin() + i * k);
  }

  return neighbors;
}

template<typename PointSource, typename PointTarget>
void FastVGICPCuda<PointSource, PointTarget>::update_correspondences(const Eigen::Isometry3d& trans) {
  vgicp_cuda->update_correspondences(trans);
}

template<typename PointSource, typename PointTarget>
void FastVGICPCuda<PointSource, PointTarget>::update_mahalanobis(const Eigen::Isometry3d& trans) {
  vgicp_cuda->update_mahalanobis(trans);
}

template<typename PointSource, typename PointTarget>
double FastVGICPCuda<PointSource, PointTarget>::compute_error(const Eigen::Isometry3d& trans, Eigen::Matrix<double, 6, 6>* H, Eigen::Matrix<double, 6, 1>* b) const {
  return vgicp_cuda->compute_error(trans, H, b);
}

}  // namespace fast_gicp

#endif
