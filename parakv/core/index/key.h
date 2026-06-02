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

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <type_traits>

namespace parakv {
namespace index {

// Key types supported by the reference index.
//
//   Key64  - 64-bit key, used by parameter-server-style workloads where the
//            key is an object id or feature sign.
//   Key128 - 128-bit key, typical for LLM KVCache where the key is a
//            collision-resistant hash (e.g. xxh3_128) over a prefix token
//            sequence.
//
// Both types are POD/trivially-copyable so they can be memcpy'd into WAL
// records and into segment slots without serialization overhead.

using Key64 = uint64_t;

struct alignas(16) Key128 {
  uint64_t hi;
  uint64_t lo;

  bool operator==(const Key128& rhs) const noexcept {
    return hi == rhs.hi && lo == rhs.lo;
  }

  bool operator!=(const Key128& rhs) const noexcept { return !(*this == rhs); }

  bool operator<(const Key128& rhs) const noexcept {
    return hi < rhs.hi || (hi == rhs.hi && lo < rhs.lo);
  }

  // Parse a 16-byte binary string into a Key128.
  // Returns true on success, false if data is null or len != 16.
  static bool FromRaw(const void* data, size_t len, Key128* out) {
    if (out == nullptr || data == nullptr || len != sizeof(Key128)) {
      return false;
    }

    std::memcpy(out, data, sizeof(Key128));

    return true;
  }

  static bool FromString(const std::string& s, Key128* out) {
    return FromRaw(s.data(), s.size(), out);
  }

  std::string ToString() const {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out(32, '0');

    for (int i = 0; i < 16; ++i) {
      uint8_t byte = static_cast<uint8_t>(hi >> (60 - i * 4)) & 0xF;
      out[i] = kHex[byte];
    }

    for (int i = 0; i < 16; ++i) {
      uint8_t byte = static_cast<uint8_t>(lo >> (60 - i * 4)) & 0xF;
      out[16 + i] = kHex[byte];
    }

    return out;
  }
};

static_assert(sizeof(Key128) == 16, "Key128 must be 16 bytes");
static_assert(std::is_trivially_copyable<Key128>::value,
              "Key128 must be trivially copyable");

// Hash functor for Key128 suitable for phmap / std::unordered_map.
//
// Mixes both halves with a 64-bit variant of the Fibonacci multiplier that
// matches the quality expected by parallel_flat_hash_map's default hasher.
struct Key128Hash {
  size_t operator()(const Key128& k) const noexcept {
    constexpr uint64_t kMix = 0x9E3779B97F4A7C15ULL;
    uint64_t h = k.hi;
    h ^= k.lo + kMix + (h << 6) + (h >> 2);
    // Final avalanche (splitmix64 step).
    h ^= h >> 30;
    h *= 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 27;
    h *= 0x94D049BB133111EBULL;
    h ^= h >> 31;
    return static_cast<size_t>(h);
  }
};

struct Key128Eq {
  bool operator()(const Key128& a, const Key128& b) const noexcept {
    return a == b;
  }
};

}  // namespace index
}  // namespace parakv

namespace std {
template <>
struct hash<::parakv::index::Key128> {
  size_t operator()(const ::parakv::index::Key128& k) const noexcept {
    return ::parakv::index::Key128Hash{}(k);
  }
};
}  // namespace std
