/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX STORAGE

#include "ob_text_daat_iter.h"

namespace oceanbase
{
namespace storage
{
int ObTextDaaTIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRDaaTIterImpl::init(*param.base_param_, *param.dim_iters_,
                                            *param.allocator_, *param.relevance_collector_))) {
  } else if (nullptr != param.base_param_->relevance_expr_
             && OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  } else {
    mode_flag_ = param.mode_flag_;
    function_lookup_mode_ = param.function_lookup_mode_;
  }
  return ret;
}

void ObTextDaaTIter::reset()
{
  bm25_param_estimator_.reset();
  // this class can know the type of dim_iters_, but ObSRDaaTIterImpl maybe not
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRDaaTIterImpl::reset();
}

void ObTextDaaTIter::reuse(const bool switch_tablet)
{
  if ((OB_NOT_NULL(dim_iters_) && 0 == dim_iters_->count())) {
    // do nothing
  } else {
    bm25_param_estimator_.reuse(switch_tablet);
  }
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRDaaTIterImpl::reuse(switch_tablet);
}

int ObTextDaaTIter::pre_process()
{
  int ret = OB_SUCCESS;
  if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (iter_param_->need_project_relevance()) {
    if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
    }
  }
  return ret;
}

int ObTextDaaTIter::do_merge_batch(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (iter_param_->need_project_relevance() || iter_param_->need_filter()
      || mode_flag_ != sql::ObMatchAgainstMode::NATURAL_LANGUAGE_MODE
      || function_lookup_mode_
      || dim_iters_->count() > ObSRSimpleMerger::USE_SIMPLE_MERGER_MAX_TABLE_CNT) {
    ret = ObSRDaaTIterImpl::do_merge_batch(capacity, count);
  } else {
    // For short existence-only unions, a direct scan of the candidates avoids
    // rebuilding a heap and transporting a score for each posting. Advance all
    // equal champions together; materialized results must be copied before
    // any next-batch load.
    const int64_t dim_count = dim_iters_->count();
    ObISRDaaTDimIter *const *const dim_iters = dim_iters_->get_data();
    const ObDatum **const domain_ids = iter_domain_ids_.get_data();
    int64_t *const next_round_iter_idxes = next_round_iter_idxes_.get_data();
    const int64_t initial_count = count;
    // Preserve cache_result's capacity error path if the buffers are smaller.
    const bool count_only = !iter_param_->materialize_results_
        && !iter_param_->limit_param_->is_valid() && count >= 0
        && capacity <= buffered_domain_ids_.count()
        && capacity <= buffered_relevances_.count();
    if (OB_UNLIKELY(dim_count < 0 || dim_count != iter_domain_ids_.count()
        || dim_count != next_round_iter_idxes_.count()
        || next_round_cnt_ < 0 || next_round_cnt_ > dim_count
        || (dim_count > 0 && (OB_ISNULL(dim_iters) || OB_ISNULL(domain_ids)
            || OB_ISNULL(next_round_iter_idxes))))) {
      ret = OB_ERR_UNEXPECTED;
    }
    while (OB_SUCC(ret) && count < capacity) {
      for (int64_t i = 0; OB_SUCC(ret) && i < next_round_cnt_; ++i) {
        const int64_t idx = next_round_iter_idxes[i];
        OB_ASSERT(idx >= 0 && idx < dim_count);
        auto *token_iter = static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters[idx]);
        if (OB_ISNULL(token_iter)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(token_iter->next_doc_id(domain_ids[idx]))) {
          if (OB_ITER_END == ret) {
            domain_ids[idx] = nullptr;
            ret = OB_SUCCESS;
          }
        }
      }
      next_round_cnt_ = 0;
      const ObDatum *champion = nullptr;
      for (int64_t i = 0; OB_SUCC(ret) && i < dim_count; ++i) {
        const ObDatum *id = domain_ids[i];
        int comparison = -1;
        if (nullptr == id) {
        } else if (nullptr != champion && OB_FAIL(merge_cmp_.compare_ids(*id, *champion, comparison))) {
        } else if (comparison <= 0) {
          if (comparison < 0) {
            champion = id;
            next_round_cnt_ = 0;
          }
          next_round_iter_idxes[next_round_cnt_++] = i;
        }
      }
      if (OB_FAIL(ret)) {
      } else if (nullptr == champion) {
        ret = OB_ITER_END;
      } else if (count_only) {
        ++count;
      } else {
        ret = cache_result(count, *champion, 1.0);
      }
    }
    if (count_only) {
      // Account rows already produced even if a later advance/compare failed.
      const int64_t produced_count = count - initial_count;
      input_row_cnt_ += produced_count;
      output_row_cnt_ += produced_count;
    }
  }
  return ret;
}

int ObTextDaaTIter::do_one_merge_round(int64_t &count)
{
  int ret = OB_SUCCESS;
  if (iter_param_->need_project_relevance() || iter_param_->need_filter()
      || mode_flag_ != sql::ObMatchAgainstMode::NATURAL_LANGUAGE_MODE
      || function_lookup_mode_) {
    ret = ObSRDaaTIterImpl::do_one_merge_round(count);
  } else if (OB_FAIL(fill_merge_heap())) {
  } else {
    // Natural-language existence predicates need only the union of document
    // IDs. Drain equal IDs without collecting a score for every posting and
    // without constructing a filter evaluation scope for every result.
    const ObSRMergeItem *item = nullptr;
    int64_t iter_idx = 0;
    bool doc_end = false;
    while (OB_SUCC(ret) && !merge_heap_->empty() && !doc_end) {
      doc_end = merge_heap_->is_unique_champion();
      if (OB_FAIL(merge_heap_->top(item))) {
      } else if (FALSE_IT(iter_idx = item->iter_idx_)) {
      } else if (OB_FAIL(merge_heap_->pop())) {
      } else {
        next_round_iter_idxes_[next_round_cnt_++] = iter_idx;
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_ISNULL(iter_domain_ids_[iter_idx])) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        ret = cache_result(count, *iter_domain_ids_[iter_idx], 1.0);
      }
    }
  }
  return ret;
}

ObTextBMWIter::ObTextBMWIter()
  : ObSRBMWIterImpl(),
    bm25_param_estimator_() {}

void ObTextBMWIter::reuse(const bool switch_tablet)
{
  bm25_param_estimator_.reuse(switch_tablet);
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRBMWIterImpl::reuse(switch_tablet);
}

void ObTextBMWIter::reset()
{
  bm25_param_estimator_.reset();
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRBMWIterImpl::reset();
}

int ObTextBMWIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRBMWIterImpl::init(*param.base_param_, *param.dim_iters_,
                                           *param.allocator_, *param.relevance_collector_))) {
  } else if (OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  }
  return ret;
}

int ObTextBMWIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
  }

  if (FAILEDx(ObSRBMWIterImpl::get_next_rows(capacity, count))) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    }
  }
  return ret;
}

int ObTextBMWIter::init_before_wand_process()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!bm25_param_estimator_.is_estimated())) {
    ret = OB_ERR_UNEXPECTED;
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
    ObTextRetrievalBlockMaxIter *block_max_iter = static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i));
    if (OB_FAIL(block_max_iter->init_block_max_iter(
        bm25_param_estimator_.get_total_doc_cnt(), bm25_param_estimator_.get_avg_doc_token_cnt()))) {
    }
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase
