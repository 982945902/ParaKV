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

#include <iostream>

namespace parakv {

enum class Status {
  kOk = 0,
  kNotFound,
  kFull,
  kIOError,
  kInvalidArgument,
  kCorruption,
  kNoSpace,
};

inline std::ostream& operator<<(std::ostream& os, Status status) {
  switch (status) {
    case Status::kOk:
      return os << "Ok";
    case Status::kNotFound:
      return os << "NotFound";
    case Status::kFull:
      return os << "Full";
    case Status::kIOError:
      return os << "IOError";
    case Status::kInvalidArgument:
      return os << "InvalidArgument";
    case Status::kCorruption:
      return os << "Corruption";
    case Status::kNoSpace:
      return os << "NoSpace";
    default:
      return os << "Unknown";
  }

  return os;
}

}  // namespace parakv

#define RETURN_IF_STATUS_NOT_OK(status) \
  if (status != Status::kOk) {          \
    return status;                      \
  }

#define RETURN_IF_STATUS_OK(status) \
  if (status == Status::kOk) {      \
    return status;                  \
  }
