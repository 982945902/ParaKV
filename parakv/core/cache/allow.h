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

// refer to
// https://github.com/jbaldwin/libcappuccino/blob/main/inc/cappuccino/allow.hpp

#pragma once

#include <cstdint>
#include <string>

namespace parakv {
/**
 * By default all `Insert()` functions will allow for inserting the key value
 * pair or updating an existing key value pair.  Pass in INSERT or UPDATE to
 * change this behavior to either only allow for inserting if the key doesn't
 * exist or only updating if the key already exists.
 */
enum class allow : uint64_t {
  /// Insertion will only succeed if the key doesn't exist.
  insert = 0x01,
  /// Insertion will only succeed if the key already exists to be updated.
  update = 0x02,
  /// Will insert or update regardless if the key exists or not.
  insert_or_update = insert | update
};

inline static auto insert_allowed(allow a) -> bool {
  return (static_cast<uint64_t>(a) & static_cast<uint64_t>(allow::insert));
}

inline static auto update_allowed(allow a) -> bool {
  return (static_cast<uint64_t>(a) & static_cast<uint64_t>(allow::update));
}

auto to_string(allow a) -> const std::string&;

}  // namespace parakv