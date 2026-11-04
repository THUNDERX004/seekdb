/* Copyright (c) 2025 OceanBase. Licensed under the Apache License, Version 2.0. */
#include <gtest/gtest.h>
#include "lib/allocator/ob_allocator.h"
#include "lib/container/ob_array.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/engine/sort/ob_sort_op_impl.h"

namespace oceanbase
{
namespace sql
{

// Invalid public init input must fail before descriptor access, in both modes.
// This tests the actual Compare::init, not an extracted/reimplemented comparator.
TEST(SortCompareInit, rejects_mismatched_descriptor_counts)
{
  common::ObArenaAllocator allocator;
  ObExecContext exec_ctx(allocator);
  const int64_t counts[][2] = {{0, 1}, {1, 0}, {3, 4}, {4, 3}, {4, 5}, {5, 4}};
  for (int64_t mode = 0; mode < 2; ++mode) {
    for (int64_t c = 0; c < 6; ++c) {
      SCOPED_TRACE(::testing::Message() << "collations=" << counts[c][0]
                   << " functions=" << counts[c][1] << " encoded=" << mode);
      common::ObArray<ObSortFieldCollation> collations;
      common::ObArray<ObSortCmpFunc> functions;
      for (int64_t i = 0; i < counts[c][0]; ++i) {
        ASSERT_EQ(OB_SUCCESS, collations.push_back(ObSortFieldCollation()));
      }
      for (int64_t i = 0; i < counts[c][1]; ++i) {
        ASSERT_EQ(OB_SUCCESS, functions.push_back(ObSortCmpFunc()));
      }
      ObSortOpImpl::Compare compare;
      EXPECT_EQ(OB_INVALID_ARGUMENT, compare.init(&collations, &functions, &exec_ctx, mode != 0));
      EXPECT_FALSE(compare.is_inited());
    }
  }
}

} // namespace sql
} // namespace oceanbase
