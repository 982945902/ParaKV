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

#include "segment_file.h"

#include <gtest/gtest.h>

#include "common/status.h"

namespace parakv {
namespace segment {

class SegmentFileTest : public ::testing::Test {
 protected:
  void SetUp() override {
    segment_file_ =
        std::make_unique<SegmentFile>(1, SegmentConfig(), "test.dat");
  }

  void TearDown() override { segment_file_->Close(); }

  std::unique_ptr<SegmentFile> segment_file_;
};

TEST_F(SegmentFileTest, TestOpen) {
  ASSERT_EQ(segment_file_->Open(), Status::kOk);
}

}  // namespace segment
}  // namespace parakv
