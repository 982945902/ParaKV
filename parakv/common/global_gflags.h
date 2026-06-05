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

#include <gflags/gflags.h>

// Segment related flags
DECLARE_uint32(segment_size);
DECLARE_uint32(segment_count);
DECLARE_uint32(segment_key_size);
DECLARE_uint32(segment_value_size);
DECLARE_uint32(segment_bitmap_alignment);
DECLARE_double(segment_compaction_threshold);
DECLARE_uint32(segment_hot_threshold);
DECLARE_uint32(segment_max_hot_count);
DECLARE_uint32(segment_flush_interval_ms);
DECLARE_string(segement_workspace_path);

// Index related flags
DECLARE_uint32(index_wal_checkpoint_bytes);

// LFU eviction flags
DECLARE_double(lfu_capacity_ratio);
DECLARE_uint32(lfu_age_tick_sec);
