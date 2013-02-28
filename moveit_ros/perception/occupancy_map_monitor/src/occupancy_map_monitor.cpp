/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2011, Willow Garage, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the Willow Garage nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/* Author: Jon Binney, Ioan Sucan */

#include <ros/ros.h>
#include <moveit_msgs/SaveMap.h>
#include <moveit_msgs/LoadMap.h>
#include <moveit/occupancy_map_monitor/occupancy_map.h>
#include <moveit/occupancy_map_monitor/occupancy_map_monitor.h>
#include <moveit/occupancy_map_monitor/point_cloud_occupancy_map_updater.h>
#include <moveit/occupancy_map_monitor/depth_image_occupancy_map_updater.h>

namespace occupancy_map_monitor
{

OccupancyMapMonitor::OccupancyMapMonitor(double map_resolution) :
  nh_("~"),
  map_resolution_(map_resolution),
  mesh_handle_count_(0),
  debug_info_(false)
{
  initialize();
}

OccupancyMapMonitor::OccupancyMapMonitor(const boost::shared_ptr<tf::Transformer> &tf, const std::string &map_frame, double map_resolution) :
  nh_("~"),
  tf_(tf),
  map_frame_(map_frame),
  map_resolution_(map_resolution),
  mesh_handle_count_(0),
  debug_info_(false)
{ 
  initialize();
}

void OccupancyMapMonitor::initialize()
{ 
  /* load params from param server */
  if (map_resolution_ <= std::numeric_limits<double>::epsilon())
    if (!nh_.getParam("octomap_resolution", map_resolution_))
    {
      map_resolution_ = 0.1;
      ROS_WARN("Resolution not specified for Octomap. Assuming resolution = %g instead", map_resolution_);
    }
  ROS_DEBUG("Using resolution = %lf m for building octomap", map_resolution_);
  
  if (map_frame_.empty())
    if (!nh_.getParam("octomap_frame", map_frame_))
      if (tf_)
        ROS_WARN("No target frame specified for Octomap. No transforms will be applied to received data.");
  
  if (!tf_ && !map_frame_.empty())
    ROS_WARN("Target frame specified but no TF instance specified. No transforms will be applied to received data.");
  
  tree_.reset(new OccMapTree(map_resolution_));
  tree_const_ = tree_;
  
  XmlRpc::XmlRpcValue sensor_list;
  if (nh_.getParam("sensors", sensor_list))
  {
    if (sensor_list.getType() == XmlRpc::XmlRpcValue::TypeArray)
      for (int32_t i = 0; i < sensor_list.size(); ++i)
      {
        if (!sensor_list[i].getType() == XmlRpc::XmlRpcValue::TypeStruct)
        {
          ROS_ERROR("Params for sensor %d not a struct, ignoring sensor", i);
          continue;
        }
        
        if (!sensor_list[i].hasMember ("sensor_type"))
        {
          ROS_ERROR("No sensor type for sensor %d; ignoring.", i);
          continue;
        }
        
        std::string sensor_type = std::string(sensor_list[i]["sensor_type"]);
        OccupancyMapUpdaterPtr up;
        if (sensor_type == "point_cloud_sensor")
          up.reset(new PointCloudOccupancyMapUpdater(this));
        else
          if (sensor_type == "depth_image_sensor" || sensor_type == "kinect")
            up.reset(new DepthImageOccupancyMapUpdater(this));
          else
          {
            ROS_ERROR("Sensor %d has unknown type %s; ignoring.", i, sensor_type.c_str());
            continue;
          }
        
        /* pass the params struct directly in to the updater */
        if (!up->setParams(sensor_list[i]))
        {
          ROS_ERROR("Failed to configure updater of type %s", up->getType().c_str());
          continue;
        }
        
        addUpdater(up);
      }
    else
      ROS_ERROR("List of sensors must be an array!");
  }
  
  /* advertise a service for loading octomaps from disk */
  save_map_srv_ = nh_.advertiseService("save_map", &OccupancyMapMonitor::saveMapCallback, this);
  load_map_srv_ = nh_.advertiseService("load_map", &OccupancyMapMonitor::loadMapCallback, this);
}

void OccupancyMapMonitor::addUpdater(const OccupancyMapUpdaterPtr &updater)
{
  if (updater && updater->initialize())
  {
    map_updaters_.push_back(updater);
    updater->setUpdateCallback(update_callback_);
    updater->publishDebugInformation(debug_info_);
    if (map_updaters_.size() > 1)
    {
      mesh_handles_.resize(map_updaters_.size()); 
      if (map_updaters_.size() == 2) // when we had one updater only, we passed direcly the transform cache callback to that updater
      {
        map_updaters_[0]->setTransformCacheCallback(boost::bind(&OccupancyMapMonitor::getShapeTransformCache, this, 0, _1, _2, _3));
        map_updaters_[1]->setTransformCacheCallback(boost::bind(&OccupancyMapMonitor::getShapeTransformCache, this, 1, _1, _2, _3));
      }
      else
        map_updaters_.back()->setTransformCacheCallback(boost::bind(&OccupancyMapMonitor::getShapeTransformCache, this, map_updaters_.size() - 1, _1, _2, _3));
    }
    else
      updater->setTransformCacheCallback(transform_cache_callback_);
  }
  else
    ROS_ERROR("Unable to initialize map updater of type %s", updater ? updater->getType().c_str() : "<NULL>");
}

void OccupancyMapMonitor::publishDebugInformation(bool flag)
{  
  debug_info_ = flag;
  for (std::size_t i = 0 ; i < map_updaters_.size() ; ++i)
    map_updaters_[i]->publishDebugInformation(debug_info_);
}

void OccupancyMapMonitor::setMapFrame(const std::string &frame)
{
  boost::mutex::scoped_lock _(parameters_lock_); // we lock since an updater could specify a new frame for us
  map_frame_ = frame;
}

void OccupancyMapMonitor::setUpdateCallback(const boost::function<void()> &update_callback)
{  
  update_callback_ = update_callback;
  for (std::size_t i = 0 ; i < map_updaters_.size() ; ++i)
    map_updaters_[i]->setUpdateCallback(update_callback_);
}

ShapeHandle OccupancyMapMonitor::excludeShape(const shapes::ShapeConstPtr &shape)
{
  // if we have just one updater, remove the additional level of indirection
  if (map_updaters_.size() == 1)
    return map_updaters_[0]->excludeShape(shape);
  
  if (map_updaters_.empty())
    return 0;
  
  ShapeHandle h = mesh_handle_count_++;
  for (std::size_t i = 0 ; i < map_updaters_.size() ; ++i)
  {
    mesh_filter::MeshHandle mh = map_updaters_[i]->excludeShape(shape);
    if (mh)
      mesh_handles_[i][h] = mh;
  }
  return h;
}

void OccupancyMapMonitor::forgetShape(ShapeHandle handle)
{
  // if we have just one updater, remove the additional level of indirection
  if (map_updaters_.size() == 1)
  {
    map_updaters_[0]->forgetShape(handle);
    return;
  }
  
  for (std::size_t i = 0 ; i < map_updaters_.size() ; ++i)
  {
    std::map<ShapeHandle, mesh_filter::MeshHandle>::const_iterator it = mesh_handles_[i].find(handle);
    if (it == mesh_handles_[i].end())
      continue;
    map_updaters_[i]->forgetShape(it->second);
  }
}

void OccupancyMapMonitor::setTransformCacheCallback(const TransformCacheProvider& transform_callback)
{
  // if we have just one updater, we connect it directly to the transform provider
  if (map_updaters_.size() == 1)
    map_updaters_[0]->setTransformCacheCallback(transform_callback);
  else
    transform_cache_callback_ = transform_callback;
}

bool OccupancyMapMonitor::getShapeTransformCache(std::size_t index, const std::string &target_frame, const ros::Time &target_time, ShapeTransformCache &cache) const
{
  if (transform_cache_callback_)
  { 
    ShapeTransformCache tempCache;
    if (transform_cache_callback_(target_frame, target_time, tempCache))
    {
      for (ShapeTransformCache::iterator it = tempCache.begin() ; it != tempCache.end() ; ++it)
      { 
        std::map<ShapeHandle, mesh_filter::MeshHandle>::const_iterator jt = mesh_handles_[index].find(it->first);
        if (jt == mesh_handles_[index].end())
        {
          ROS_ERROR_THROTTLE(1, "Incorrect mapping of mesh handles");
          return false;
        }
        else
          cache[jt->second] = it->second;
      }
      return true;
    }
    else
      return false;
  }
  else
    return false;
}

bool OccupancyMapMonitor::saveMapCallback(moveit_msgs::SaveMap::Request& request, moveit_msgs::SaveMap::Response& response)
{
  ROS_INFO("Writing map to %s", request.filename.c_str());
  tree_->lockRead();
  try
  {
    response.success = tree_->writeBinary(request.filename);
  }
  catch (...)
  {
    response.success = false;
  }
  tree_->unlockRead();
  return true;
}

bool OccupancyMapMonitor::loadMapCallback(moveit_msgs::LoadMap::Request& request, moveit_msgs::LoadMap::Response& response)
{
  ROS_INFO("Reading map from %s", request.filename.c_str());
  
  /* load the octree from disk */
  tree_->lockWrite();
  try
  {
    response.success = tree_->readBinary(request.filename);
  }
  catch (...)
  { 
    ROS_ERROR("Failed to load map from file");
    response.success = false;
  }
  tree_->unlockWrite();
  
  return true;
}

void OccupancyMapMonitor::startMonitor()
{
  /* initialize all of the occupancy map updaters */
  for (std::size_t i = 0 ; i < map_updaters_.size() ; ++i)
    map_updaters_[i]->start();
}

void OccupancyMapMonitor::stopMonitor()
{  
  for (std::size_t i = 0 ; i < map_updaters_.size() ; ++i)
    map_updaters_[i]->stop();
}

OccupancyMapMonitor::~OccupancyMapMonitor()
{
  stopMonitor();
}

}
