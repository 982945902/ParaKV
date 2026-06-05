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

#include "global_gflags.h"

// Segment related flags
DEFINE_uint32(segment_size, 64 * 1024 * 1024, "segment size in bytes");
DEFINE_uint32(segment_count, 3, "total number of segments");
DEFINE_uint32(segment_key_size, 8, "fixed key size in bytes");
DEFINE_uint32(segment_value_size, 512, "fixed value size in bytes");
DEFINE_uint32(segment_bitmap_alignment, 4096, "bitmap area alignment");
DEFINE_double(segment_compaction_threshold, 0.75,
              "deleted slot ratio to trigger compaction");
DEFINE_uint32(segment_hot_threshold, 1000,
              "access count threshold to promote segment to hot");
DEFINE_uint32(segment_max_hot_count, 64,
              "maximum number of hot segments in memory");
DEFINE_uint32(segment_flush_interval_ms, 1000,
              "hot segment async flush interval in milliseconds");
DEFINE_string(segement_workspace_path, "./segment_workspace",
              "segment workspace path");

// Index related flags
DEFINE_uint32(index_wal_checkpoint_bytes, 1ULL << 29,
              "index wal checkpoint bytes, default to 512MB");

// LFU eviction flags
DEFINE_double(lfu_capacity_ratio, 0.9,
              "LFU capacity = total_segment_slots * ratio. "
              "When the number of entries exceeds this, the least frequently "
              "used entry is evicted. 0 disables LFU eviction.");
DEFINE_uint32(lfu_age_tick_sec, 60,
              "Dynamic aging interval in seconds for LFU cache. Items not "
              "accessed within this interval have their frequency halved.");
