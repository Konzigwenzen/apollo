/******************************************************************************
 * Copyright 2017 The Apollo Authors. All Rights Reserved.
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

/**
 * @file
 **/

#include "modules/planning/tasks/path_decider/path_decider.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "modules/planning/proto/decision.pb.h"

#include "modules/common/configs/vehicle_config_helper.h"
#include "modules/common/util/util.h"
#include "modules/planning/common/planning_gflags.h"

namespace apollo {
namespace planning {

using apollo::common::ErrorCode;
using apollo::common::Status;
using apollo::common::VehicleConfigHelper;

PathDecider::PathDecider() : Task("PathDecider") {}

apollo::common::Status PathDecider::Execute(
    Frame *, ReferenceLineInfo *reference_line_info) {
  Task::Execute(nullptr, reference_line_info);
  return Process(reference_line_info->path_data(),
                 reference_line_info->path_decision());
}

Status PathDecider::Process(const PathData &path_data,
                            PathDecision *const path_decision) {
  CHECK_NOTNULL(path_decision);
  if (!MakeObjectDecision(path_data, path_decision)) {
    AERROR << "Failed to make decision based on tunnel";
    return Status(ErrorCode::PLANNING_ERROR, "dp_road_graph decision ");
  }
  return Status::OK();
}

bool PathDecider::MakeObjectDecision(const PathData &path_data,
                                     PathDecision *const path_decision) {
  DCHECK_NOTNULL(path_decision);
  if (!MakeStaticObstacleDecision(path_data, path_decision)) {
    AERROR << "Failed to make decisions for static obstacles";
    return false;
  }
  return true;
}

bool PathDecider::MakeStaticObstacleDecision(
    const PathData &path_data, PathDecision *const path_decision) {
  DCHECK_NOTNULL(path_decision);
  const auto &frenet_path = path_data.frenet_frame_path();
  const auto &frenet_points = frenet_path.points();
  if (frenet_points.empty()) {
    AERROR << "Path is empty.";
    return false;
  }

  const double half_width =
      common::VehicleConfigHelper::GetConfig().vehicle_param().width() / 2.0;

  const double lateral_radius = half_width + FLAGS_lateral_ignore_buffer;

  const double lateral_stop_radius =
      half_width + FLAGS_static_decision_nudge_l_buffer;

  for (const auto *path_obstacle : path_decision->path_obstacles().Items()) {
    const auto &obstacle = *path_obstacle->obstacle();
    if (!obstacle.IsStatic()) {
      continue;
    }
    if (path_obstacle->HasLongitudinalDecision() &&
        path_obstacle->LongitudinalDecision().has_ignore() &&
        path_obstacle->HasLateralDecision() &&
        path_obstacle->LateralDecision().has_ignore()) {
      continue;
    }
    if (path_obstacle->HasLongitudinalDecision() &&
        path_obstacle->LongitudinalDecision().has_stop()) {
      continue;
    }

    if (path_obstacle->st_boundary().boundary_type() ==
        StBoundary::BoundaryType::KEEP_CLEAR) {
      continue;
    }

    // IGNORE by default
    ObjectDecisionType object_decision;
    object_decision.mutable_ignore();

    const auto &sl_boundary = path_obstacle->perception_sl_boundary();

    if (sl_boundary.start_s() < frenet_points.front().s() ||
        sl_boundary.start_s() > frenet_points.back().s()) {
      path_decision->AddLongitudinalDecision("PathDecider", obstacle.Id(),
                                             object_decision);
      path_decision->AddLateralDecision("PathDecider", obstacle.Id(),
                                        object_decision);
      continue;
    }

    const auto frenet_point = frenet_path.EvaluateByS(sl_boundary.start_s());
    const double curr_l = frenet_point.l();
    if (curr_l - lateral_radius > sl_boundary.end_l() ||
        curr_l + lateral_radius < sl_boundary.start_l()) {
      // ignore
      path_decision->AddLateralDecision("PathDecider", obstacle.Id(),
                                        object_decision);
    } else if (curr_l - lateral_stop_radius < sl_boundary.end_l() &&
               curr_l + lateral_stop_radius > sl_boundary.start_l()) {
      *object_decision.mutable_stop() =
          GenerateObjectStopDecision(*path_obstacle);
      path_decision->AddLongitudinalDecision("PathDecider", obstacle.Id(),
                                             object_decision);
    } else if (FLAGS_enable_nudge_decision &&
               (curr_l - lateral_stop_radius > sl_boundary.end_l())) {
      ObjectNudge *object_nudge_ptr = object_decision.mutable_nudge();
      object_nudge_ptr->set_type(ObjectNudge::LEFT_NUDGE);
      object_nudge_ptr->set_distance_l(FLAGS_nudge_distance_obstacle);
      path_decision->AddLateralDecision("PathDecider", obstacle.Id(),
                                        object_decision);
    } else {
      if (FLAGS_enable_nudge_decision) {
        ObjectNudge *object_nudge_ptr = object_decision.mutable_nudge();
        object_nudge_ptr->set_type(ObjectNudge::RIGHT_NUDGE);
        object_nudge_ptr->set_distance_l(-FLAGS_nudge_distance_obstacle);
        path_decision->AddLateralDecision("PathDecider", obstacle.Id(),
                                          object_decision);
      }
    }
  }

  return true;
}

double PathDecider::MinimumRadiusStopDistance(
    const PathObstacle &path_obstacle) const {
  constexpr double stop_distance_buffer = 0.5;
  const auto &vehicle_param = VehicleConfigHelper::GetConfig().vehicle_param();
  const double min_turn_radius = VehicleConfigHelper::MinSafeTurnRadius();
  double lateral_diff =
      std::max(std::fabs(path_obstacle.perception_sl_boundary().start_l() -
                         reference_line_info_->AdcSlBoundary().end_l()),
               std::fabs(path_obstacle.perception_sl_boundary().end_l() -
                         reference_line_info_->AdcSlBoundary().start_l()));
  lateral_diff = std::max(lateral_diff, vehicle_param.width());
  lateral_diff = std::min(lateral_diff,
                          vehicle_param.width() +
                              path_obstacle.perception_sl_boundary().end_l() -
                              path_obstacle.perception_sl_boundary().start_l());
  double stop_distance = std::sqrt(min_turn_radius * min_turn_radius -
                                   (min_turn_radius - lateral_diff) *
                                       (min_turn_radius - lateral_diff)) +
                         stop_distance_buffer;
  stop_distance = std::min(stop_distance, FLAGS_max_stop_distance_obstacle);
  stop_distance = std::max(stop_distance, FLAGS_min_stop_distance_obstacle);
  return stop_distance;
}

ObjectStop PathDecider::GenerateObjectStopDecision(
    const PathObstacle &path_obstacle) const {
  ObjectStop object_stop;

  double stop_distance = FLAGS_max_stop_distance_obstacle;

  if (path_obstacle.obstacle()->Id() == FLAGS_destination_obstacle_id) {
    // destination
    object_stop.set_reason_code(StopReasonCode::STOP_REASON_DESTINATION);
    stop_distance = FLAGS_stop_distance_destination;
  } else {
    stop_distance = MinimumRadiusStopDistance(path_obstacle);
    object_stop.set_reason_code(StopReasonCode::STOP_REASON_OBSTACLE);
  }
  object_stop.set_distance_s(-stop_distance);

  const double stop_ref_s =
      path_obstacle.perception_sl_boundary().start_s() - stop_distance;
  const auto stop_ref_point =
      reference_line_info_->reference_line().GetReferencePoint(stop_ref_s);
  object_stop.mutable_stop_point()->set_x(stop_ref_point.x());
  object_stop.mutable_stop_point()->set_y(stop_ref_point.y());
  object_stop.set_stop_heading(stop_ref_point.heading());
  return object_stop;
}

}  // namespace planning
}  // namespace apollo
