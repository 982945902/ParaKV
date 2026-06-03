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
// https://github.com/jbaldwin/libcappuccino/blob/main/inc/cappuccino/lock.hpp

#pragma once

#include <mutex>
#include <string>

namespace parakv {
/**
 * Determines if the cache should use thread safe synchronization or not.
 * By default all caches use synchronization for multi-threaded accesses.
 * By using thread_safe::no you can disable locking internally on the caches,
 * this should really only be used if you implemented your own locking strategy
 * or the cache in question is owned and used by a single thread.
 */
enum class thread_safe {
  /// No synchronization, not thread safe but faster if you are using 1
  /// dedicated thread.
  no = 0,
  /// Thread safe synchronization via locks.
  yes = 1,
};

auto to_string(thread_safe ts) -> const std::string&;

/**
 * Creates a lock that based on the thread_safety will behave correctly.
 * thread_safe::yes => Uses a std::mutex
 * thread_safe::no => is a no-op.
 *
 * @tparam thread_safe_type The thread_safe type to use.
 * @tparam lock_type The underlying lock type to use.  Must support .lock() and
 * .unlock().
 */
template <thread_safe thread_safe_type, typename lock_type = std::mutex>
class mutex {
 public:
  constexpr auto lock() -> void {
    if constexpr (thread_safe_type == thread_safe::yes) {
      m_lock.lock();
    }
  }

  constexpr auto unlock() -> void {
    if constexpr (thread_safe_type == thread_safe::yes) {
      m_lock.unlock();
    }
  }

 private:
  lock_type m_lock;
};

}  // namespace parakv
