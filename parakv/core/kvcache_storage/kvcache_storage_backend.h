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

#include <cstdint>
#include <string>

namespace parakv {
namespace kvcache_storage {

// Backend-facing return code. The service layer translates this into the
// wire-level parakv.proto.StatusCode so that the storage backend does not
// need to depend on protobuf types.
enum class BackendCode : int {
  kOk = 0,
  kNotFound,
  kAlreadyExists,
  kValueTooLarge,
  kStorageFull,
  kIOError,
  kUnavailable,
  kCorrupted,
  kInvalidArgument,
};

struct WriteOptions {
  bool overwrite = false;
  bool durable = false;
  uint64_t ttl_ms = 0;  // 0 means "backend default".
};

struct ReadOptions {
  bool include_value = true;
  bool touch_on_hit = false;
};

// Per-item outcome returned by the backend.
struct WriteResult {
  BackendCode code = BackendCode::kOk;
  std::string message;
};

struct ReadResult {
  BackendCode code = BackendCode::kNotFound;
  std::string message;
  std::string value;
  std::string metadata;
  uint64_t expiration_ms = 0;
};

// Abstract backend interface. Implementations are expected to be
// thread-safe; the service layer calls them from brpc worker threads.
class KVCacheStorageBackend {
 public:
  virtual ~KVCacheStorageBackend() = default;

  // Write one entry. `key`, `value`, `metadata` are owned by the caller.
  virtual WriteResult Put(const std::string& ns, const std::string& key,
                          const std::string& value, const std::string& metadata,
                          const WriteOptions& opts) = 0;

  // Read one entry. On miss the backend must set `code == kNotFound` and
  // leave `value`/`metadata` empty.
  virtual ReadResult Get(const std::string& ns, const std::string& key,
                         const ReadOptions& opts) = 0;
};

}  // namespace kvcache_storage
}  // namespace parakv
