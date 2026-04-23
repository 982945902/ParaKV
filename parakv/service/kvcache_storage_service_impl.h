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
#include <memory>

#include "core/kvcache_storage/kvcache_storage_backend.h"
#include "kvcache_storage_service.pb.h"

namespace parakv {
namespace service {

struct ServiceOptions {
  // Upper bound on items per BatchWrite / keys per BatchRead. Requests
  // larger than this are rejected with INVALID_ARGUMENT.
  uint32_t max_batch_size = 1024;

  // Upper bound on a single value blob in bytes. 0 means "no service-side
  // limit" (the backend may still reject oversized blobs).
  uint64_t max_value_bytes = 0;
};

// brpc / protobuf generic service implementation of
// parakv.proto.KVCacheStorageService.
class KVCacheStorageServiceImpl final
    : public parakv::proto::KVCacheStorageService {
 public:
  explicit KVCacheStorageServiceImpl(
      std::shared_ptr<kvcache_storage::KVCacheStorageBackend> backend);
  KVCacheStorageServiceImpl(
      std::shared_ptr<kvcache_storage::KVCacheStorageBackend> backend,
      ServiceOptions options);
  ~KVCacheStorageServiceImpl() override;

  KVCacheStorageServiceImpl(const KVCacheStorageServiceImpl&) = delete;
  KVCacheStorageServiceImpl& operator=(const KVCacheStorageServiceImpl&) =
      delete;

  void BatchWrite(google::protobuf::RpcController* cntl_base,
                  const parakv::proto::BatchWriteRequest* request,
                  parakv::proto::BatchWriteResponse* response,
                  google::protobuf::Closure* done) override;

  void BatchRead(google::protobuf::RpcController* cntl_base,
                 const parakv::proto::BatchReadRequest* request,
                 parakv::proto::BatchReadResponse* response,
                 google::protobuf::Closure* done) override;

 private:
  std::shared_ptr<kvcache_storage::KVCacheStorageBackend> backend_;
  ServiceOptions options_;
};

}  // namespace service
}  // namespace parakv
