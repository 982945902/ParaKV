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
// https://github.com/jbaldwin/libcappuccino/blob/main/inc/cappuccino/lfuda_cache.hpp

#pragma once

#include <chrono>
#include <functional>
#include <list>
#include <mutex>
#include <vector>

#include "parakv/core/cache/allow.h"
#include "parakv/core/cache/lock.h"
#include "parallel_hashmap/btree.h"
#include "parallel_hashmap/phmap.h"

namespace parakv {
/**
 * Least Frequently Used (LFU) Cache with Dynamic Aging.
 * Each key value pair is evicted based on being the least frequently used, and
 * no other criteria.  However, items will dynamically age based on a time tick
 * frequency and a ratio to reduce its use count.  This means that items that
 * are hot for a short while won't stick around in the cache with high use
 * counts as they will dynamically age out over a period of time.  The dynamic
 * age tick count as well as the ratio can be tuned by the user of the cache.
 *
 * This cache is thread_safe aware and can be used concurrently from multiple
 * threads safely. To remove locks/synchronization use NO when creating the
 * cache.
 *
 * @tparam key_type The key type.  Must support Hash and KeyEqual.
 * @tparam value_type The value type.  This is returned by copy on a find, so if
 * your data structure value is large it is advisable to store in a shared ptr.
 * @tparam thread_safe_type By default this cache is thread safe, can be
 * disabled for caches specific to a single thread.
 * @tparam Hash Hash function for key_type.
 * @tparam KeyEqual Equality comparator for key_type.
 */
template <typename key_type, typename value_type,
          thread_safe thread_safe_type = thread_safe::yes,
          typename Hash = phmap::priv::hash_default_hash<key_type>,
          typename KeyEqual = phmap::priv::hash_default_eq<key_type>>
class lfuda_cache {
 private:
  struct element;

  using age_iterator = typename std::list<element>::iterator;

 public:
  using eviction_callback = std::function<void(const key_type&, value_type&)>;

  /**
   * @param capacity The maximum number of key value pairs allowed in the cache.
   * @param dynamic_age_tick The amount of time to pass before dynamically aging
   * items. Items that haven't been used for this amount of time will have their
   * use count reduced by the dynamic age ratio value.
   * @param dynamic_age_ratio The 'used' amount to age items by when they
   * dynamically age. The default is to halve their use count.
   * @param max_load_factor The load factor for the hash map, generally 1 is a
   * good default.
   * @param on_evict Optional callback invoked when an item is evicted due to
   * capacity overflow.
   */
  explicit lfuda_cache(
      size_t capacity,
      std::chrono::milliseconds dynamic_age_tick = std::chrono::minutes{1},
      float dynamic_age_ratio = 0.5f, float max_load_factor = 1.0f,
      eviction_callback on_evict = nullptr)
      : m_on_evict(std::move(on_evict)),
        m_dynamic_age_tick(dynamic_age_tick),
        m_dynamic_age_ratio(dynamic_age_ratio),
        m_dynamic_age_list(capacity) {
    m_open_list_end = m_dynamic_age_list.begin();

    m_keyed_elements.max_load_factor(max_load_factor);
    m_keyed_elements.reserve(capacity);
  }

  /**
   * Inserts or updates the given key value pair.
   * Note that this function will attempt to dynamically age as many items as it
   * can that are ready to be dynamically aged.  Worst case running time is O(n)
   * if every item was inserted into the cache with the same timestamp.
   * @param key The key to store the value under.
   * @param value The value of the data to store.
   * @param a Allowed methods of insertion | update.  Defaults to allowing
   *              insertions and updates.
   * @return True if the operation was successful based on `allow`.
   */
  auto insert(const key_type& key, value_type value,
              allow a = allow::insert_or_update) -> bool {
    auto now = std::chrono::steady_clock::now();

    std::lock_guard guard{m_lock};
    return do_insert_update(key, std::move(value), now, a);
  }

  /**
   * Inserts or updates a range of key value pairs.  This expects a container
   * that has 2 values in the {key_type, value_type} ordering.
   * There is a simple struct provided on the lfuda_cache::KeyValue that can be
   * put into any iterable container to satisfy this requirement.
   *
   * Note that this function might make future Inserts extremely expensive if
   * it is inserting / updating a lot of items at once.  This is because each
   * item inserted will have the same dynamic age timestamp and could cause
   * Inserts to be very expensive when all of those items dynamically age at the
   * same time.  User beware.
   *
   * @tparam range_type A container with two items, key_type, value_type.
   * @param key_value_range The elements to insert or update into the cache.
   * @param a Allowed methods of insertion | update.  Defaults to allowing
   *              insertions and updates.
   * @return The number of elements inserted based on `allow`.
   */
  template <typename range_type>
  auto insert_range(range_type&& key_value_range,
                    allow a = allow::insert_or_update) -> size_t {
    auto now = std::chrono::steady_clock::now();

    size_t inserted{0};

    {
      std::lock_guard guard{m_lock};
      for (auto& [key, value] : key_value_range) {
        if (do_insert_update(key, std::move(value), now, a)) {
          ++inserted;
        }
      }
    }

    return inserted;
  }

  /**
   * Attempts to delete the given key.
   * @param key The key to delete from the lru cache.
   * @return True if the key was deleted, false if the key does not exist.
   */
  auto erase(const key_type& key) -> bool {
    std::lock_guard guard{m_lock};
    auto keyed_position = m_keyed_elements.find(key);
    if (keyed_position != m_keyed_elements.end()) {
      do_erase(keyed_position->second);
      return true;
    } else {
      return false;
    }
  }

  /**
   * Attempts to delete all given keys.
   * @tparam range_type A container with the set of keys to delete, e.g.
   * vector<key_type>, set<key_type>.
   * @param key_range The keys to delete from the cache.
   * @return The number of items deleted from the cache.
   */
  template <typename range_type>
  auto erase_range(const range_type& key_range) -> size_t {
    size_t deleted_elements{0};

    std::lock_guard guard{m_lock};
    for (auto& key : key_range) {
      auto keyed_position = m_keyed_elements.find(key);
      if (keyed_position != m_keyed_elements.end()) {
        ++deleted_elements;
        do_erase(keyed_position->second);
      }
    }

    return deleted_elements;
  }

  /**
   * Attempts to find the given key's value.
   * @param key The key to lookup its value.
   * @param peek Should the find act like the item wasn't used?
   * @return An optional with the key's value if it exists, or an empty optional
   * if it does not.
   */
  auto find(const key_type& key, bool peek = false)
      -> std::optional<value_type> {
    auto now = std::chrono::steady_clock::now();

    std::lock_guard guard{m_lock};
    return do_find(key, peek, now);
  }

  /**
   * Attempts to find the given key's value and use count.
   * @param key The key to lookup its value.
   * @param peek Should the find act like the item wasn't used?
   * @return An optional with the key's value and use count if it exists, or
   * empty optional if it does not.
   */
  auto find_with_use_count(const key_type& key, bool peek = false)
      -> std::optional<std::pair<value_type, size_t>> {
    auto now = std::chrono::steady_clock::now();

    std::lock_guard guard{m_lock};
    return do_find_with_use_count(key, peek, now);
  }

  /**
   * Attempts to find all the given keys values.
   * @tparam range_type A container with the set of keys to find their values,
   * e.g. vector<key_type>.
   * @param key_range The keys to lookup their pairs.
   * @param peek Should the find act like all the items were not used?
   * @return The full set of keys to std::nullopt if the key wasn't found, or
   * the value if found.
   */
  template <typename range_type>
  auto find_range(const range_type& key_range, bool peek = false)
      -> std::vector<std::pair<key_type, std::optional<value_type>>> {
    auto now = std::chrono::steady_clock::now();

    std::vector<std::pair<key_type, std::optional<value_type>>> output;
    output.reserve(std::size(key_range));

    {
      std::lock_guard guard{m_lock};
      for (auto& key : key_range) {
        output.emplace_back(key, do_find(key, peek, now));
      }
    }

    return output;
  }

  /**
   * Attempts to find all the given keys values.
   *
   * The user should initialize this container with the keys to lookup with the
   * values as all empty optionals.  The keys that are found will have the
   * optionals filled in with the appropriate values from the cache.
   *
   * @tparam range_type A container with a pair of optional items,
   *                   e.g. vector<pair<key_type, optional<value_type>>>
   *                   or map<key_type, optional<value_type>>
   * @param key_optional_value_range The keys to optional values to fill out.
   * @param peek Should the find act like all the items were not used?
   */
  template <typename range_type>
  auto find_range_fill(range_type& key_optional_value_range, bool peek = false)
      -> void {
    auto now = std::chrono::steady_clock::now();

    std::lock_guard guard{m_lock};
    for (auto& [key, optional_value] : key_optional_value_range) {
      optional_value = do_find(key, peek, now);
    }
  }

  /**
   * Attempts to dynamically age the elements in the cache.
   * This function has a worst case running time of O(n) IIF every element is
   * inserted with the same timestamp -- as it will dynamically age every item
   * in the cache if they are all ready to be aged.
   * @return The number of items dynamically aged.
   */
  auto dynamically_age() -> size_t {
    auto now = std::chrono::steady_clock::now();

    std::lock_guard guard{m_lock};
    return do_dynamic_age(now);
  }

  /**
   * @return If this cache is currenty empty.
   */
  auto empty() const -> bool { return (m_used_size == 0); }

  /**
   * @return The number of elements inside the cache.
   */
  auto size() const -> size_t { return m_used_size; }

  /**
   * @return The maximum capacity of this cache.
   */
  auto capacity() const -> size_t { return m_dynamic_age_list.size(); }

 private:
  struct element {
    key_type m_key;
    size_t m_use_count{0};
    std::chrono::steady_clock::time_point m_dynamic_age;
    value_type m_value;
  };

  void erase_lfu_entry(size_t use_count, age_iterator target) {
    auto [lo, hi] = m_lfu_list.equal_range(use_count);
    for (auto it = lo; it != hi; ++it) {
      if (it->second == target) {
        m_lfu_list.erase(it);
        return;
      }
    }
  }

  auto do_insert_update(const key_type& key, value_type&& value,
                        std::chrono::steady_clock::time_point now, allow a)
      -> bool {
    auto keyed_position = m_keyed_elements.find(key);
    if (keyed_position != m_keyed_elements.end()) {
      if (update_allowed(a)) {
        do_update(keyed_position->second, std::move(value), now);
        return true;
      }
    } else {
      if (insert_allowed(a)) {
        do_insert(key, std::move(value), now);
        return true;
      }
    }

    return false;
  }

  auto do_insert(const key_type& key, value_type&& value,
                 std::chrono::steady_clock::time_point now) -> void {
    if (m_used_size >= m_dynamic_age_list.size()) {
      do_prune(now);
    }

    element& e = *m_open_list_end;

    m_keyed_elements.emplace(key, m_open_list_end);
    m_lfu_list.emplace(1, m_open_list_end);

    e.m_key = key;
    e.m_use_count = 1;
    e.m_value = std::move(value);
    e.m_dynamic_age = now;

    ++m_open_list_end;

    ++m_used_size;
  }

  auto do_update(age_iterator pos, value_type&& value,
                 std::chrono::steady_clock::time_point now) -> void {
    element& e = *pos;
    e.m_value = std::move(value);

    do_access(e, pos, now);
  }

  auto do_erase(age_iterator age_it) -> void {
    element& e = *age_it;

    if (age_it != std::prev(m_open_list_end)) {
      m_dynamic_age_list.splice(m_open_list_end, m_dynamic_age_list, age_it);
    }
    --m_open_list_end;

    m_keyed_elements.erase(e.m_key);
    erase_lfu_entry(e.m_use_count, age_it);

    --m_used_size;
  }

  auto do_find(const key_type& key, bool peek,
               std::chrono::steady_clock::time_point now)
      -> std::optional<value_type> {
    auto keyed_position = m_keyed_elements.find(key);
    if (keyed_position != m_keyed_elements.end()) {
      age_iterator age_it = keyed_position->second;
      element& e = *age_it;
      if (!peek) {
        do_access(e, age_it, now);
      }
      return {e.m_value};
    }

    return {};
  }

  auto do_find_with_use_count(const key_type& key, bool peek,
                              std::chrono::steady_clock::time_point now)
      -> std::optional<std::pair<value_type, size_t>> {
    auto keyed_position = m_keyed_elements.find(key);
    if (keyed_position != m_keyed_elements.end()) {
      age_iterator age_it = keyed_position->second;
      element& e = *age_it;
      if (!peek) {
        do_access(e, age_it, now);
      }
      return {std::make_pair(e.m_value, e.m_use_count)};
    }

    return {};
  }

  auto do_access(element& e, age_iterator age_it,
                 std::chrono::steady_clock::time_point now) -> void {
    erase_lfu_entry(e.m_use_count, age_it);
    ++e.m_use_count;
    m_lfu_list.emplace(e.m_use_count, age_it);

    auto last_aged_item = std::prev(m_open_list_end);
    if (age_it != last_aged_item) {
      m_dynamic_age_list.splice(last_aged_item, m_dynamic_age_list, age_it);
    }
    e.m_dynamic_age = now;
  }

  auto do_prune(std::chrono::steady_clock::time_point now) -> void {
    if (m_used_size > 0) {
      do_dynamic_age(now);

      auto victim = m_lfu_list.begin()->second;
      if (m_on_evict) {
        element& e = *victim;
        m_on_evict(e.m_key, e.m_value);
      }
      do_erase(victim);
    }
  }

  auto do_dynamic_age(std::chrono::steady_clock::time_point now) -> size_t {
    size_t aged{0};

    auto da_start = m_dynamic_age_list.begin();
    auto da_last = m_open_list_end;
    while (da_start != m_open_list_end &&
           (*da_start).m_dynamic_age + m_dynamic_age_tick < now) {
      if (da_start != da_last) {
        m_dynamic_age_list.splice(da_last, m_dynamic_age_list, da_start);
      }

      element& e = *da_start;
      e.m_dynamic_age = now;

      erase_lfu_entry(e.m_use_count, da_start);
      e.m_use_count = (size_t)(e.m_use_count * m_dynamic_age_ratio);
      m_lfu_list.emplace(e.m_use_count, da_start);

      da_last = da_start;
      da_start = m_dynamic_age_list.begin();
      ++aged;
    }

    return aged;
  }

  /// Optional eviction callback.
  eviction_callback m_on_evict;

  /// Cache lock for all mutations if thread_safe is enabled.
  mutex<thread_safe_type> m_lock;

  /// The current number of elements in the cache.
  size_t m_used_size{0};

  /// The amount of time for an element to not be touched to dynamically age.
  std::chrono::milliseconds m_dynamic_age_tick{std::chrono::minutes{1}};
  /// The ratio amount of 'uses' to remove when an element dynamically ages.
  float m_dynamic_age_ratio{0.5f};

  /// The keyed lookup data structure, maps key -> age_iterator.
  /// Uses phmap::parallel_flat_hash_map without internal mutex (NullMutex).
  phmap::parallel_flat_hash_map<key_type, age_iterator, Hash, KeyEqual>
      m_keyed_elements;
  /// The lfu sorted map, the key is the number of times the element has been
  /// used, the value is the age_iterator.
  phmap::btree_multimap<size_t, age_iterator> m_lfu_list;
  /// The dynamic age list, this also contains a partition on un-used
  /// 'm_elements'.
  std::list<element> m_dynamic_age_list;
  /// The end of the open list to pull open slots from.
  age_iterator m_open_list_end;
};

}  // namespace parakv
