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

#include "backend_registry.h"

#include <glog/logging.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "backend_namespace_manager.h"
#include "common/global_gflags.h"
#include "index_kvcache_storage_backend.h"
#include "mocked_inmemory_kvcache_storage.h"

namespace parakv {
namespace kvcache_storage {

const std::string BackendConfig::kEmpty;

void BackendConfig::Set(std::string key, std::string value) {
  entries_[std::move(key)] = std::move(value);
}

bool BackendConfig::Has(const std::string& key) const {
  return entries_.find(key) != entries_.end();
}

const std::string& BackendConfig::Get(const std::string& key,
                                      const std::string& default_value) const {
  auto it = entries_.find(key);
  return it == entries_.end() ? default_value : it->second;
}

int64_t BackendConfig::GetInt(const std::string& key,
                              int64_t default_value) const {
  auto it = entries_.find(key);
  if (it == entries_.end() || it->second.empty()) {
    return default_value;
  }

  errno = 0;
  char* end = nullptr;
  const long long v = std::strtoll(it->second.c_str(), &end, 10);
  if (errno != 0 || end == it->second.c_str() || *end != '\0') {
    LOG(WARNING) << "BackendConfig: cannot parse int for key=" << key
                 << " value=" << it->second << ", using default";
    return default_value;
  }

  return static_cast<int64_t>(v);
}

uint64_t BackendConfig::GetUint(const std::string& key,
                                uint64_t default_value) const {
  auto it = entries_.find(key);
  if (it == entries_.end() || it->second.empty()) {
    return default_value;
  }

  errno = 0;
  char* end = nullptr;
  const unsigned long long v = std::strtoull(it->second.c_str(), &end, 10);
  if (errno != 0 || end == it->second.c_str() || *end != '\0') {
    LOG(WARNING) << "BackendConfig: cannot parse uint for key=" << key
                 << " value=" << it->second << ", using default";
    return default_value;
  }

  return static_cast<uint64_t>(v);
}

bool BackendConfig::GetBool(const std::string& key, bool default_value) const {
  auto it = entries_.find(key);
  if (it == entries_.end()) {
    return default_value;
  }

  std::string v = it->second;
  std::transform(v.begin(), v.end(), v.begin(),
                 [](unsigned char c) { return std::tolower(c); });

  if (v == "1" || v == "true" || v == "yes" || v == "on") {
    return true;
  }

  if (v == "0" || v == "false" || v == "no" || v == "off" || v.empty()) {
    return false;
  }

  LOG(WARNING) << "BackendConfig: cannot parse bool for key=" << key
               << " value=" << it->second << ", using default";

  return default_value;
}

BackendRegistry& BackendRegistry::Instance() {
  static BackendRegistry instance;

  return instance;
}

bool BackendRegistry::Register(std::string name, BackendFactory factory) {
  LOG(INFO) << "BackendRegistry::Register: registering backend '" << name
            << "'";

  if (name.empty()) {
    LOG(ERROR) << "BackendRegistry::Register rejected: empty name";
    return false;
  }
  if (!factory) {
    LOG(ERROR) << "BackendRegistry::Register rejected: null factory for "
               << name;
    return false;
  }

  std::lock_guard<std::mutex> lock(mu_);
  auto [it, inserted] = factories_.emplace(std::move(name), std::move(factory));
  if (!inserted) {
    LOG(WARNING) << "BackendRegistry: backend '" << it->first
                 << "' already registered; keeping the existing entry";
    return false;
  }

  VLOG(1) << "BackendRegistry: registered backend '" << it->first << "'";
  return true;
}

std::shared_ptr<KVCacheStorageBackend> BackendRegistry::Create(
    const std::string& name, const BackendConfig& config) const {
  BackendFactory factory;
  {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = factories_.find(name);
    if (it == factories_.end()) {
      LOG(ERROR) << "BackendRegistry::Create: unknown backend '" << name << "'";
      return nullptr;
    }
    factory = it->second;
  }

  auto backend = factory(config);
  if (!backend) {
    LOG(ERROR) << "BackendRegistry::Create: factory for '" << name
               << "' returned nullptr";
  }

  return backend;
}

bool BackendRegistry::IsRegistered(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mu_);

  return factories_.find(name) != factories_.end();
}

std::vector<std::string> BackendRegistry::RegisteredNames() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<std::string> names;
  names.reserve(factories_.size());
  for (const auto& kv : factories_) {
    names.push_back(kv.first);
  }
  std::sort(names.begin(), names.end());

  return names;
}

}  // namespace kvcache_storage
}  // namespace parakv

PARAKV_REGISTER_KVCACHE_BACKEND(
    "kvcache_memory",
    [](const ::parakv::kvcache_storage::BackendConfig& cfg)
        -> std::shared_ptr<::parakv::kvcache_storage::KVCacheStorageBackend> {
      ::parakv::kvcache_storage::MockedInMemoryKVCacheStorage::Options opts;
      opts.max_value_bytes = cfg.GetUint("max_value_bytes", 0);
      opts.default_ttl_ms = cfg.GetUint("default_ttl_ms", 0);

      return std::make_shared<
          ::parakv::kvcache_storage::MockedInMemoryKVCacheStorage>(opts);
    });

PARAKV_REGISTER_KVCACHE_BACKEND(
    "kvcache_index128",
    [](const ::parakv::kvcache_storage::BackendConfig& cfg)
        -> std::shared_ptr<::parakv::kvcache_storage::KVCacheStorageBackend> {
      const auto ns = cfg.Get("namespace", "default");
      const auto sanitized =
          ::parakv::kvcache_storage::BackendNamespaceManager::SanitizeNs(ns);

      ::parakv::kvcache_storage::IndexKVCacheStorageBackend::Options opts;
      opts.root_dir =
          std::string(FLAGS_segement_workspace_path) + "/" + sanitized;
      opts.namespace_name = ns;
      opts.segment_size = FLAGS_segment_size;
      opts.slot_value_size = FLAGS_segment_value_size;
      opts.segment_count = FLAGS_segment_count;
      opts.wal_checkpoint_bytes = FLAGS_index_wal_checkpoint_bytes;
      opts.lfu_capacity_ratio = FLAGS_lfu_capacity_ratio;
      opts.lfu_age_tick_sec = FLAGS_lfu_age_tick_sec;

      return std::make_shared<
          ::parakv::kvcache_storage::IndexKVCacheStorageBackend>(opts);
    });
