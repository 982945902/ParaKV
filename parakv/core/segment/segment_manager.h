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

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "segment_base.h"

namespace parakv {
namespace segment {

struct HotSegmentInfo {
  uint32_t segment_id;
  uint64_t access_count;
  std::vector<uint8_t> cached_data;
};

class SegmentManager {
 public:
  explicit SegmentManager(const SegmentConfig& config);
  ~SegmentManager();

  SegmentManager(const SegmentManager&) = delete;
  SegmentManager& operator=(const SegmentManager&) = delete;

  // Register an externally created segment.
  Status AddSegment(std::shared_ptr<SegmentBase> segment);

  // Returns an APPENDING segment that has free slots.
  // If none exists, promotes an IDLE segment.
  std::shared_ptr<SegmentBase> GetActiveSegment();

  // Return segment by id.
  std::shared_ptr<SegmentBase> GetSegment(uint32_t segment_id);

  // Release a segment to IDLE pool (after compaction).
  Status ReleaseSegment(uint32_t segment_id);

  // ---- State persistence ----

  // Save the manager's classification state (idle / appending / full order)
  // to a binary file so it can be restored across service restarts.
  Status SaveState(const std::string& path) const;

  // Load a previously persisted state file and reclassify segments
  // accordingly.  Must be called AFTER all segments have been added via
  // AddSegment().  Segment IDs present in the file but missing from the
  // manager are silently skipped.
  Status LoadState(const std::string& path);

  // ---- Compaction ----

  void SetSlotMoveCallback(SlotMoveCallback cb) {
    slot_move_cb_ = std::move(cb);
  }

  // Scan all FULL segments and compact those exceeding the deletion threshold.
  Status RunCompaction();

  // ---- Hot / Cold management ----

  void RecordAccess(uint32_t segment_id);

  // Promote / demote segments based on accumulated access counts.
  // hot_threshold: min access count to promote.
  // max_hot_segments: memory budget expressed as max number of hot segments.
  Status EvaluateHotCold(uint64_t hot_threshold, uint32_t max_hot_segments);

  bool IsHotSegment(uint32_t segment_id) const;

  // ---- Queries ----

  uint32_t GetTotalSegments() const;
  uint32_t GetIdleSegments() const;
  uint32_t GetFullSegments() const;

  // Return FULL segment IDs in the order they became full (oldest first).
  std::vector<uint32_t> GetFullSegmentOrder() const;

 private:
  std::shared_ptr<SegmentBase> PromoteIdleSegment();

  SegmentConfig config_;

  mutable std::mutex mutex_;

  std::unordered_map<uint32_t, std::shared_ptr<SegmentBase>> segments_;

  std::unordered_set<uint32_t> idle_set_;
  std::unordered_set<uint32_t> appending_set_;

  // FULL segments: full_set_ for O(1) membership test, full_order_ preserves
  // the chronological order in which segments became full (oldest first).
  std::unordered_set<uint32_t> full_set_;
  std::vector<uint32_t> full_order_;

  // Hot segment tracking
  std::unordered_map<uint32_t, uint64_t> access_counters_;
  std::unordered_set<uint32_t> hot_segments_;

  SlotMoveCallback slot_move_cb_;
};

}  // namespace segment
}  // namespace parakv
