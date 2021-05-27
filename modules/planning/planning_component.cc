/******************************************************************************
 * Copyright 2018 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/
#include "modules/planning/planning_component.h"

#include "cyber/common/file.h"
#include "modules/common/adapters/adapter_gflags.h"
#include "modules/common/configs/config_gflags.h"
#include "modules/common/util/message_util.h"
#include "modules/common/util/util.h"
#include "modules/map/hdmap/hdmap_util.h"
#include "modules/map/pnc_map/pnc_map.h"
#include "modules/planning/common/history.h"
#include "modules/planning/common/planning_context.h"
#include "modules/planning/navi_planning.h"
#include "modules/planning/on_lane_planning.h"

#include "modules/common/math/quaternion.h"
#include "modules/common/time/time.h"

namespace apollo {
namespace planning {

using apollo::cyber::ComponentBase;
using apollo::hdmap::HDMapUtil;
using apollo::perception::TrafficLightDetection;
using apollo::relative_map::MapMsg;
using apollo::routing::RoutingRequest;
using apollo::routing::RoutingResponse;
using apollo::storytelling::Stories;

bool PlanningComponent::Init() {
  injector_ = std::make_shared<DependencyInjector>();

  if (FLAGS_use_navigation_mode) {
    planning_base_ = std::make_unique<NaviPlanning>(injector_);
  } else {
    planning_base_ = std::make_unique<OnLanePlanning>(injector_);
  }

  ACHECK(ComponentBase::GetProtoConfig(&config_))
      << "failed to load planning config file "
      << ComponentBase::ConfigFilePath();

  if (FLAGS_planning_learning_mode > 0) {
    if (!message_process_.Init(config_)) {
      AERROR << "failed to init MessageProcess";
      return false;
    }
  }

  planning_base_->Init(config_);

  routing_reader_ = node_->CreateReader<RoutingResponse>(
      config_.topic_config().routing_response_topic(),
      [this](const std::shared_ptr<RoutingResponse>& routing) {
        AINFO << "Received routing data: run routing callback."
              << routing->header().DebugString();
        std::lock_guard<std::mutex> lock(mutex_);
        routing_.CopyFrom(*routing);
      });

  traffic_light_reader_ = node_->CreateReader<TrafficLightDetection>(
      config_.topic_config().traffic_light_detection_topic(),
      [this](const std::shared_ptr<TrafficLightDetection>& traffic_light) {
        ADEBUG << "Received traffic light data: run traffic light callback.";
        std::lock_guard<std::mutex> lock(mutex_);
        traffic_light_.CopyFrom(*traffic_light);
      });

  pad_msg_reader_ = node_->CreateReader<PadMessage>(
      config_.topic_config().planning_pad_topic(),
      [this](const std::shared_ptr<PadMessage>& pad_msg) {
        ADEBUG << "Received pad data: run pad callback.";
        std::lock_guard<std::mutex> lock(mutex_);
        pad_msg_.CopyFrom(*pad_msg);
      });

  story_telling_reader_ = node_->CreateReader<Stories>(
      config_.topic_config().story_telling_topic(),
      [this](const std::shared_ptr<Stories>& stories) {
        ADEBUG << "Received story_telling data: run story_telling callback.";
        std::lock_guard<std::mutex> lock(mutex_);
        stories_.CopyFrom(*stories);
      });

  if (FLAGS_use_navigation_mode) {
    relative_map_reader_ = node_->CreateReader<MapMsg>(
        config_.topic_config().relative_map_topic(),
        [this](const std::shared_ptr<MapMsg>& map_message) {
          ADEBUG << "Received relative map data: run relative map callback.";
          std::lock_guard<std::mutex> lock(mutex_);
          relative_map_.CopyFrom(*map_message);
        });
  }
  planning_writer_ = node_->CreateWriter<ADCTrajectory>(
      config_.topic_config().planning_trajectory_topic());

  rerouting_writer_ = node_->CreateWriter<RoutingRequest>(
      config_.topic_config().routing_request_topic());

  return true;
}

bool PlanningComponent::Proc(
    const std::shared_ptr<prediction::PredictionObstacles>&
        prediction_obstacles,
    const std::shared_ptr<canbus::Chassis>& chassis,
    const std::shared_ptr<localization::LocalizationEstimate>&
        localization_estimate) {

  AINFO << "exp:planning_start";

  ACHECK(prediction_obstacles != nullptr);

  // check and process possible rerouting request
  CheckRerouting();


  // process fused input data
  local_view_.prediction_obstacles = prediction_obstacles;
  local_view_.chassis = chassis;
  local_view_.localization_estimate = localization_estimate;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!local_view_.routing ||
        hdmap::PncMap::IsNewRouting(*local_view_.routing, routing_)) {
      local_view_.routing =
          std::make_shared<routing::RoutingResponse>(routing_);
    }
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    local_view_.traffic_light =
        std::make_shared<TrafficLightDetection>(traffic_light_);
    local_view_.relative_map = std::make_shared<MapMsg>(relative_map_);
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    local_view_.pad_msg = std::make_shared<PadMessage>(pad_msg_);
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    local_view_.stories = std::make_shared<Stories>(stories_);
  }

  
  //added by me
  // AINFO << "exp:ego_speed_from_chasis "
  //       << local_view_.chassis->speed_mps() << "\n"
    AINFO << "exp:self_car_intent "
        << local_view_.prediction_obstacles->intent().type() << "\n"
        << "exp:scenario "
        << local_view_.prediction_obstacles->scenario().type() << "\n"
        << "exp:obs_num "
        << local_view_.prediction_obstacles->prediction_obstacle_size() << "\n";
  int ob=0;
  for(const auto &obs: local_view_.prediction_obstacles->prediction_obstacle()){
    AINFO <<"exp:ob"<< ob <<"_static "
            // <<obs.DebugString();
          << obs.is_static() << "\n"
          << "exp:ob"<< ob <<"_intent "
          << obs.intent().type() << "\n"
          << "exp:ob"<< ob <<"_priority "
          << obs.priority().priority() << "\n"
          <<"exp:ob"<< ob <<"_heading "
          <<obs.perception_obstacle().theta()<<"\n"
          << "exp:ob"<< ob <<"_vx:ob"<< ob <<"_vy:ob"<< ob <<"_vz "
          <<obs.perception_obstacle().velocity().x() << " "
          <<obs.perception_obstacle().velocity().y() << " "
          <<obs.perception_obstacle().velocity().z() << "\n"
          << "exp:ob"<< ob <<"_ax:ob"<< ob <<"_ay:ob"<< ob <<"_az "
          <<obs.perception_obstacle().acceleration().x() << " "
          <<obs.perception_obstacle().acceleration().y() << " "
          <<obs.perception_obstacle().acceleration().z() << " ";
          // << obs.perception_obstacle().type();
    ob+=1;
  }

  if (!CheckInput()) {
    AERROR << "Input check failed";
    return false;
  }

  if (FLAGS_planning_learning_mode == 2 || FLAGS_planning_learning_mode == 3) {
    // data process for online training
    message_process_.OnChassis(*local_view_.chassis);
    message_process_.OnPrediction(*local_view_.prediction_obstacles);
    message_process_.OnRoutingResponse(*local_view_.routing);
    message_process_.OnStoryTelling(*local_view_.stories);
    message_process_.OnTrafficLightDetection(*local_view_.traffic_light);
    message_process_.OnLocalization(*local_view_.localization_estimate);
  }

  double start_timestamp=apollo::common::time::Clock::NowInSeconds();

  ADCTrajectory adc_trajectory_pb;
  planning_base_->RunOnce(local_view_, &adc_trajectory_pb);

  AINFO << "exp:planning_duration "<<apollo::common::time::Clock::NowInSeconds()-start_timestamp<< '\n';

  auto start_time = adc_trajectory_pb.header().timestamp_sec();
  common::util::FillHeader(node_->Name(), &adc_trajectory_pb);

  // modify trajectory relative time due to the timestamp change in header
  const double dt = start_time - adc_trajectory_pb.header().timestamp_sec();
  for (auto& p : *adc_trajectory_pb.mutable_trajectory_point()) {
    p.set_relative_time(p.relative_time() + dt);
  }
  planning_writer_->Write(adc_trajectory_pb);

  // record in history
  auto* history = injector_->history();
  history->Add(adc_trajectory_pb);

  apollo::common::VehicleState vehicle_state = injector_->vehicle_state()->vehicle_state();
  apollo::common::Point3D v=vehicle_state.pose().linear_velocity();
  Eigen::Vector3d vec1(v.x(),
                      v.y(),
                      v.z());
  // Eigen::Vector3d vec1=common::math::QuaternionRotate(vehicle_state.pose().orientation(),orig);
  // Eigen::Vector3d vec2=common::math::QuaternionRotate(vehicle_state.pose().orientation(),orig);

  AINFO << "exp:speed:acceleration:heading "
        << vehicle_state.linear_velocity() << " " 
        << vehicle_state.linear_acceleration() <<" "
        << vehicle_state.heading()<<"\n"
        << "exp:vx:vy:vz "
        << vec1[0] << " " <<vec1[1]<< " " <<vec1[2]<< " " <<"\n"
        <<"exp:ax_vrf:ay_vrf:az_vrf "
        << vehicle_state.pose().linear_acceleration_vrf().x()<<" "
        << vehicle_state.pose().linear_acceleration_vrf().y()<<" "
        << vehicle_state.pose().linear_acceleration_vrf().z()<<"\n"
        <<"exp:planning_end";

  return true;
}

void PlanningComponent::CheckRerouting() {
  auto* rerouting = injector_->planning_context()
                        ->mutable_planning_status()
                        ->mutable_rerouting();
  if (!rerouting->need_rerouting()) {
    return;
  }
  common::util::FillHeader(node_->Name(), rerouting->mutable_routing_request());
  rerouting->set_need_rerouting(false);
  rerouting_writer_->Write(rerouting->routing_request());
}

bool PlanningComponent::CheckInput() {
  ADCTrajectory trajectory_pb;
  auto* not_ready = trajectory_pb.mutable_decision()
                        ->mutable_main_decision()
                        ->mutable_not_ready();

  if (local_view_.localization_estimate == nullptr) {
    not_ready->set_reason("localization not ready");
  } else if (local_view_.chassis == nullptr) {
    not_ready->set_reason("chassis not ready");
  } else if (HDMapUtil::BaseMapPtr() == nullptr) {
    not_ready->set_reason("map not ready");
  } else {
    // nothing
  }

  if (FLAGS_use_navigation_mode) {
    if (!local_view_.relative_map->has_header()) {
      not_ready->set_reason("relative map not ready");
    }
  } else {
    if (!local_view_.routing->has_header()) {
      not_ready->set_reason("routing not ready");
    }
  }

  if (not_ready->has_reason()) {
    AERROR << not_ready->reason() << "; skip the planning cycle.";
    common::util::FillHeader(node_->Name(), &trajectory_pb);
    planning_writer_->Write(trajectory_pb);
    return false;
  }
  return true;
}

}  // namespace planning
}  // namespace apollo
