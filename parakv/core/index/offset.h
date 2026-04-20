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

#include <cassert>
#include <cstdint>

namespace parakv {
namespace index {

// Encoded offset layout (64 bits):
//
//   bit [63:61]  -> 3-bit flag (OffsetFlag)
//   bit [60: 0]  -> flag specific payload
//
// Flag semantics (from docs/zh/design/index.md):
//   000 kFileOffset  : low 48 bits = file block byte offset (<= 256TB)
//   001 kSegmentSlot : low 48 bits = segment_id(20) | slot_id(28)
//   010 kMemory      : low 48 bits = in-memory pointer (hot cache)
//
// Leaving 13 unused bits in each encoding reserved for future extensions
// (version / generation / tier tag etc.).
enum class OffsetFlag : uint8_t {
  kFileOffset = 0,
  kSegmentSlot = 1,
  kMemory = 2,
};

class OffsetCodec {
 public:
  static constexpr int kFlagBits = 3;
  static constexpr int kFlagShift = 61;
  static constexpr uint64_t kFlagMask = (uint64_t{1} << kFlagBits) - 1;

  static constexpr int kSlotIdBits = 28;
  static constexpr int kSegmentIdBits = 20;
  static constexpr uint64_t kSlotIdMask = (uint64_t{1} << kSlotIdBits) - 1;
  static constexpr uint64_t kSegmentIdMask =
      (uint64_t{1} << kSegmentIdBits) - 1;

  static constexpr int kFileOffsetBits = 48;
  static constexpr uint64_t kFileOffsetMask =
      (uint64_t{1} << kFileOffsetBits) - 1;

  static constexpr int kMemoryAddrBits = 48;
  static constexpr uint64_t kMemoryAddrMask =
      (uint64_t{1} << kMemoryAddrBits) - 1;

  // Special reserved "invalid" encoded offset.
  static constexpr uint64_t kInvalid = UINT64_MAX;

  static uint64_t EncodeFileOffset(uint64_t file_offset) {
    assert((file_offset & ~kFileOffsetMask) == 0);
    return (static_cast<uint64_t>(OffsetFlag::kFileOffset) << kFlagShift) |
           (file_offset & kFileOffsetMask);
  }

  static uint64_t EncodeSegmentSlot(uint32_t segment_id, uint32_t slot_id) {
    assert((static_cast<uint64_t>(segment_id) & ~kSegmentIdMask) == 0);
    assert((static_cast<uint64_t>(slot_id) & ~kSlotIdMask) == 0);
    uint64_t payload =
        (static_cast<uint64_t>(segment_id) << kSlotIdBits) | slot_id;
    return (static_cast<uint64_t>(OffsetFlag::kSegmentSlot) << kFlagShift) |
           payload;
  }

  static uint64_t EncodeMemoryAddress(const void* ptr) {
    uint64_t addr = reinterpret_cast<uint64_t>(ptr);
    assert((addr & ~kMemoryAddrMask) == 0);
    return (static_cast<uint64_t>(OffsetFlag::kMemory) << kFlagShift) |
           (addr & kMemoryAddrMask);
  }

  static OffsetFlag GetFlag(uint64_t encoded) {
    return static_cast<OffsetFlag>((encoded >> kFlagShift) & kFlagMask);
  }

  static uint64_t GetFileOffset(uint64_t encoded) {
    return encoded & kFileOffsetMask;
  }

  static uint32_t GetSegmentId(uint64_t encoded) {
    return static_cast<uint32_t>((encoded >> kSlotIdBits) & kSegmentIdMask);
  }

  static uint32_t GetSlotId(uint64_t encoded) {
    return static_cast<uint32_t>(encoded & kSlotIdMask);
  }

  static void* GetMemoryAddress(uint64_t encoded) {
    return reinterpret_cast<void*>(encoded & kMemoryAddrMask);
  }
};

}  // namespace index
}  // namespace parakv
