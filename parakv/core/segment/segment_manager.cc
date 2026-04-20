/* Copyright 2026 The ParaKV Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://gitcode.com/xLLM-AI/ParaKV/blob/main/LICENSE

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "segment_manager.h"

#include <algorithm>
#include <cassert>

namespace parakv {
namespace segment {

SegmentManager::SegmentManager(const SegmentConfig& config) : config_(config) {}

SegmentManager::~SegmentManager() = default;

Status SegmentManager::AddSegment(std::shared_ptr<SegmentBase> segment) {
  if (!segment) {
    return Status::kInvalidArgument;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  uint32_t id = segment->GetSegmentId();
  if (segments_.count(id)) {
    return Status::kInvalidArgument;
  }

  SegmentState state = segment->GetState();
  segments_[id] = std::move(segment);

  switch (state) {
    case SegmentState::IDLE:
      idle_set_.insert(id);
      break;
    case SegmentState::APPENDING:
      appending_set_.insert(id);
      break;
    case SegmentState::FULL:
      full_set_.insert(id);
      break;
  }

  return Status::kOk;
}

std::shared_ptr<SegmentBase> SegmentManager::GetActiveSegment() {
  std::lock_guard<std::mutex> lock(mutex_);

  // Try to find an existing APPENDING segment with free slots
  for (uint32_t id : appending_set_) {
    auto seg = segments_[id];
    if (seg->GetFreeSlots() > 0) {
      return seg;
    }
  }

  // Promote an IDLE segment
  return PromoteIdleSegment();
}

std::shared_ptr<SegmentBase> SegmentManager::GetSegment(uint32_t segment_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = segments_.find(segment_id);
  if (it == segments_.end()) {
    return nullptr;
  }

  return it->second;
}

Status SegmentManager::ReleaseSegment(uint32_t segment_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = segments_.find(segment_id);
  if (it == segments_.end()) {
    return Status::kNotFound;
  }

  appending_set_.erase(segment_id);
  full_set_.erase(segment_id);
  hot_segments_.erase(segment_id);
  access_counters_.erase(segment_id);
  idle_set_.insert(segment_id);

  return Status::kOk;
}

std::shared_ptr<SegmentBase> SegmentManager::PromoteIdleSegment() {
  if (idle_set_.empty()) {
    return nullptr;
  }

  uint32_t id = *idle_set_.begin();
  idle_set_.erase(id);
  appending_set_.insert(id);

  return segments_[id];
}

Status SegmentManager::RunCompaction() {
  std::vector<uint32_t> candidates;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (uint32_t id : full_set_) {
      // Skip hot segments
      if (hot_segments_.count(id)) continue;
      auto seg = segments_[id];
      if (seg->NeedsCompaction()) {
        candidates.push_back(id);
      }
    }
  }

  for (uint32_t src_id : candidates) {
    auto active = GetActiveSegment();
    if (!active) {
      return Status::kNoSpace;
    }

    std::shared_ptr<SegmentBase> src;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = segments_.find(src_id);
      if (it == segments_.end()) {
        continue;
      }

      src = it->second;
    }

    auto s = src->Compact(active.get());
    if (s != Status::kOk) {
      return s;
    }

    // Move source segment to IDLE
    {
      std::lock_guard<std::mutex> lock(mutex_);
      full_set_.erase(src_id);
      idle_set_.insert(src_id);

      // Reclassify target based on its current state
      uint32_t target_id = active->GetSegmentId();
      SegmentState new_state = active->GetState();
      if (new_state == SegmentState::FULL) {
        appending_set_.erase(target_id);
        full_set_.insert(target_id);
      }
    }
  }

  return Status::kOk;
}

void SegmentManager::RecordAccess(uint32_t segment_id) {
  std::lock_guard<std::mutex> lock(mutex_);

  access_counters_[segment_id]++;
}

Status SegmentManager::EvaluateHotCold(uint64_t hot_threshold,
                                       uint32_t max_hot_segments) {
  std::lock_guard<std::mutex> lock(mutex_);

  // Promote cold -> hot
  for (auto& [id, count] : access_counters_) {
    if (count >= hot_threshold && !hot_segments_.count(id) &&
        hot_segments_.size() < max_hot_segments) {
      hot_segments_.insert(id);
    }
  }

  // Demote hot segments whose access dropped below threshold
  std::vector<uint32_t> to_demote;
  for (uint32_t id : hot_segments_) {
    auto it = access_counters_.find(id);
    if (it == access_counters_.end() || it->second < hot_threshold) {
      to_demote.push_back(id);
    }
  }
  for (uint32_t id : to_demote) {
    hot_segments_.erase(id);
    // If the demoted segment has high deletion ratio, it becomes
    // a compaction candidate on the next RunCompaction() call.
  }

  // Enforce memory budget: if too many hot segments, evict least-accessed
  while (hot_segments_.size() > max_hot_segments) {
    uint32_t victim = 0;
    uint64_t min_count = UINT64_MAX;
    for (uint32_t id : hot_segments_) {
      uint64_t c = access_counters_[id];
      if (c < min_count) {
        min_count = c;
        victim = id;
      }
    }
    hot_segments_.erase(victim);
  }

  // Reset counters for next evaluation window
  access_counters_.clear();

  return Status::kOk;
}

bool SegmentManager::IsHotSegment(uint32_t segment_id) const {
  std::lock_guard<std::mutex> lock(mutex_);

  return hot_segments_.count(segment_id) > 0;
}

uint32_t SegmentManager::GetTotalSegments() const {
  std::lock_guard<std::mutex> lock(mutex_);

  return static_cast<uint32_t>(segments_.size());
}

uint32_t SegmentManager::GetIdleSegments() const {
  std::lock_guard<std::mutex> lock(mutex_);

  return static_cast<uint32_t>(idle_set_.size());
}

uint32_t SegmentManager::GetFullSegments() const {
  std::lock_guard<std::mutex> lock(mutex_);

  return static_cast<uint32_t>(full_set_.size());
}

}  // namespace segment
}  // namespace parakv
