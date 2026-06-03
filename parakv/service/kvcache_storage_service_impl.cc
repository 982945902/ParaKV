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

#include "kvcache_storage_service_impl.h"

#include <brpc/closure_guard.h>
#include <brpc/controller.h>
#include <glog/logging.h>

#include <utility>

#include "core/index/key.h"

namespace parakv {
namespace service {

namespace {

using parakv::proto::BatchDeleteRequest;
using parakv::proto::BatchDeleteResponse;
using parakv::proto::BatchDeleteResult;
using parakv::proto::BatchReadRequest;
using parakv::proto::BatchReadResponse;
using parakv::proto::BatchReadResult;
using parakv::proto::BatchWriteRequest;
using parakv::proto::BatchWriteResponse;
using parakv::proto::BatchWriteResult;
using parakv::proto::KVItem;
using parakv::proto::StatusCode;

using parakv::kvcache_storage::BackendCode;
using parakv::kvcache_storage::ReadOptions;
using parakv::kvcache_storage::ReadResult;
using parakv::kvcache_storage::WriteOptions;
using parakv::kvcache_storage::WriteResult;

StatusCode ToProtoCode(BackendCode code) {
  switch (code) {
    case BackendCode::kOk:
      return StatusCode::OK;
    case BackendCode::kNotFound:
      return StatusCode::NOT_FOUND;
    case BackendCode::kAlreadyExists:
      return StatusCode::ALREADY_EXISTS;
    case BackendCode::kValueTooLarge:
      return StatusCode::VALUE_TOO_LARGE;
    case BackendCode::kStorageFull:
      return StatusCode::STORAGE_FULL;
    case BackendCode::kIOError:
      return StatusCode::IO_ERROR;
    case BackendCode::kUnavailable:
      return StatusCode::UNAVAILABLE;
    case BackendCode::kCorrupted:
      return StatusCode::DATA_CORRUPTED;
    case BackendCode::kInvalidArgument:
      return StatusCode::INVALID_ARGUMENT;
  }
  return StatusCode::UNKNOWN;
}

void FillItemError(BatchWriteResult* r, const std::string& key, StatusCode code,
                   const std::string& msg) {
  r->set_key(key);
  r->set_code(code);
  r->set_message(msg);
}

void FillItemError(BatchReadResult* r, const std::string& key, StatusCode code,
                   const std::string& msg) {
  r->set_key(key);
  r->set_code(code);
  r->set_message(msg);
}

void FillItemError(BatchDeleteResult* r, const std::string& key,
                   StatusCode code, const std::string& msg) {
  r->set_key(key);
  r->set_code(code);
  r->set_message(msg);
}

}  // namespace

KVCacheStorageServiceImpl::KVCacheStorageServiceImpl(
    std::shared_ptr<kvcache_storage::BackendNamespaceManager> manager)
    : KVCacheStorageServiceImpl(std::move(manager), ServiceOptions{}) {}

KVCacheStorageServiceImpl::KVCacheStorageServiceImpl(
    std::shared_ptr<kvcache_storage::BackendNamespaceManager> manager,
    ServiceOptions options)
    : manager_(std::move(manager)), options_(options) {
  CHECK(manager_ != nullptr)
      << "KVCacheStorageServiceImpl requires a namespace manager";
}

KVCacheStorageServiceImpl::~KVCacheStorageServiceImpl() = default;

void KVCacheStorageServiceImpl::BatchWrite(
    google::protobuf::RpcController* cntl_base,
    const BatchWriteRequest* request, BatchWriteResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  (void)cntl;

  const int n = request->items_size();
  VLOG(1) << "BatchWrite request_id=" << request->request_id()
          << " ns=" << request->namespace_() << " items=" << n
          << " overwrite=" << request->overwrite();

  if (n == 0) {
    response->set_code(StatusCode::INVALID_ARGUMENT);
    response->set_message("empty batch");
    return;
  }
  if (static_cast<uint32_t>(n) > options_.max_batch_size) {
    response->set_code(StatusCode::INVALID_ARGUMENT);
    response->set_message("batch too large");
    return;
  }

  response->mutable_results()->Reserve(n);

  WriteOptions base_opts;
  base_opts.overwrite = request->overwrite();
  base_opts.durable = request->durable();

  uint32_t success = 0;
  uint32_t failure = 0;

  for (const KVItem& item : request->items()) {
    auto* out = response->add_results();

    if (item.key().empty()) {
      FillItemError(out, item.key(), StatusCode::INVALID_ARGUMENT, "empty key");
      ++failure;
      continue;
    }
    if (options_.max_value_bytes > 0 &&
        item.value().size() > options_.max_value_bytes) {
      FillItemError(out, item.key(), StatusCode::VALUE_TOO_LARGE,
                    "value exceeds service limit");
      ++failure;
      continue;
    }

    WriteOptions opts = base_opts;
    opts.ttl_ms = item.ttl_ms();

    WriteResult r = manager_->Put(request->namespace_(), item.key(),
                                  item.value(), item.metadata(), opts);
    out->set_key(item.key());
    out->set_code(ToProtoCode(r.code));
    if (!r.message.empty()) out->set_message(r.message);

    index::Key128 k;
    if (r.code == BackendCode::kOk) {
      if (index::Key128::FromString(item.key(), &k)) {
        LOG(INFO) << "Write success: key=" << k.ToString();
      } else {
        LOG(INFO) << "Write success: key=" << item.key();
      }
      ++success;
    } else {
      if (index::Key128::FromString(item.key(), &k)) {
        LOG(ERROR) << "Write failed: key=" << k.ToString()
                   << " code=" << static_cast<int>(r.code);
      } else {
        LOG(ERROR) << "Write failed: key=" << item.key()
                   << " code=" << static_cast<int>(r.code);
      }
      //  << " message=" << r.message;
      ++failure;
    }
  }

  response->set_success_count(success);
  response->set_failure_count(failure);
  // Request-level code stays OK: the batch itself was processed. Callers must
  // still inspect per-item `results` to see which items succeeded.
  response->set_code(StatusCode::OK);
}

void KVCacheStorageServiceImpl::BatchRead(
    google::protobuf::RpcController* cntl_base, const BatchReadRequest* request,
    BatchReadResponse* response, google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  (void)cntl;

  const int n = request->keys_size();
  VLOG(1) << "BatchRead request_id=" << request->request_id()
          << " ns=" << request->namespace_() << " keys=" << n
          << " include_value=" << request->include_value();

  if (n == 0) {
    response->set_code(StatusCode::INVALID_ARGUMENT);
    response->set_message("empty batch");
    return;
  }
  if (static_cast<uint32_t>(n) > options_.max_batch_size) {
    response->set_code(StatusCode::INVALID_ARGUMENT);
    response->set_message("batch too large");
    return;
  }

  response->mutable_results()->Reserve(n);

  ReadOptions opts;
  opts.include_value = request->include_value();
  opts.touch_on_hit = request->touch_on_hit();

  uint32_t hit = 0;
  uint32_t miss = 0;

  for (const std::string& key : request->keys()) {
    auto* out = response->add_results();

    if (key.empty()) {
      FillItemError(out, key, StatusCode::INVALID_ARGUMENT, "empty key");
      ++miss;
      continue;
    }

    ReadResult r = manager_->Get(request->namespace_(), key, opts);
    out->set_key(key);
    out->set_code(ToProtoCode(r.code));
    if (!r.message.empty()) out->set_message(r.message);
    out->set_expiration_ms(r.expiration_ms);

    if (r.code == BackendCode::kOk) {
      if (opts.include_value) {
        out->set_value(std::move(r.value));
      }
      if (!r.metadata.empty()) {
        out->set_metadata(std::move(r.metadata));
      }
      ++hit;
    } else {
      ++miss;
    }
  }

  response->set_hit_count(hit);
  response->set_miss_count(miss);
  response->set_code(StatusCode::OK);
}

void KVCacheStorageServiceImpl::BatchDelete(
    google::protobuf::RpcController* cntl_base,
    const BatchDeleteRequest* request, BatchDeleteResponse* response,
    google::protobuf::Closure* done) {
  brpc::ClosureGuard done_guard(done);
  auto* cntl = static_cast<brpc::Controller*>(cntl_base);
  (void)cntl;

  const int n = request->keys_size();
  VLOG(1) << "BatchDelete request_id=" << request->request_id()
          << " ns=" << request->namespace_() << " keys=" << n;

  if (n == 0) {
    response->set_code(StatusCode::INVALID_ARGUMENT);
    response->set_message("empty batch");
    return;
  }
  if (static_cast<uint32_t>(n) > options_.max_batch_size) {
    response->set_code(StatusCode::INVALID_ARGUMENT);
    response->set_message("batch too large");
    return;
  }

  response->mutable_results()->Reserve(n);

  uint32_t success = 0;
  uint32_t failure = 0;

  for (const std::string& key : request->keys()) {
    auto* out = response->add_results();

    if (key.empty()) {
      FillItemError(out, key, StatusCode::INVALID_ARGUMENT, "empty key");
      ++failure;
      continue;
    }

    WriteResult r = manager_->Delete(request->namespace_(), key);
    out->set_key(key);
    out->set_code(ToProtoCode(r.code));
    if (!r.message.empty()) out->set_message(r.message);

    if (r.code == BackendCode::kOk) {
      ++success;
    } else {
      ++failure;
    }
  }

  response->set_success_count(success);
  response->set_failure_count(failure);
  response->set_code(StatusCode::OK);
}

}  // namespace service
}  // namespace parakv
