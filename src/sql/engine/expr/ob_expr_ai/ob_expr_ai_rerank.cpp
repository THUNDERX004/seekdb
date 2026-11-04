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

#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_ai_rerank.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include "lib/utility/utility.h"
#include "common/json_type/ob_json_common.h"
#include <algorithm>
#include <cmath>
 
using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{

ObExprAIRerank::ObExprAIRerank(common::ObIAllocator &alloc)
: ObFuncExprOperator(alloc, 
                    T_FUN_SYS_AI_RERANK, 
                    N_AI_RERANK, 
                    MORE_THAN_ZERO,
                    NOT_VALID_FOR_GENERATED_COL, 
                    NOT_ROW_DIMENSION) 
{
}

ObExprAIRerank::~ObExprAIRerank() 
{
}

int ObExprAIRerank::calc_result_typeN(ObExprResType &type,
                                    ObExprResType *types_stack,
                                    int64_t param_num,
                                    common::ObExprTypeCtx &type_ctx) const {
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num < 3 || param_num > 4)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    if (!ob_is_string_tc(types_stack[MODEL_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model key must be string type");
    } else if (!ob_is_string_tc(types_stack[QUERY_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, query must be string type");
    } else {
      types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      types_stack[QUERY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    }
    ObObjType in_type = types_stack[DOCUMENTS_IDX].get_type();
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types_stack, DOCUMENTS_IDX, N_AI_RERANK))) {
    } else if (ob_is_string_type(in_type) && types_stack[DOCUMENTS_IDX].get_collation_type() != CS_TYPE_BINARY) {
      if (types_stack[DOCUMENTS_IDX].get_charset_type() != CHARSET_UTF8MB4) {
        types_stack[DOCUMENTS_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (param_num == 4) {
      ObObjType doc_key_type = types_stack[DOC_KEY_IDX].get_type();
      if (doc_key_type == ObNullType) {
      } else if (doc_key_type == ObJsonType) {
      } else if (ob_is_string_type(doc_key_type)) {
        types_stack[DOC_KEY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      } else {  
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, doc key must be string type");
      }
    }

    if (OB_SUCC(ret)) {
      type.set_json();
      type.set_length((ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType]).get_length());
    }
  }
  return ret;
}

int ObExprAIRerank::eval_ai_rerank(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_query = nullptr;
  ObDatum *arg_documents = nullptr;
  ObDatum *arg_doc_key = nullptr;
  if (OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_query, arg_documents, arg_doc_key))) {
  } else if (arg_model_id->is_null() || arg_query->is_null() || arg_documents->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model id or query or documents is null");
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_RERANK));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    const ObString model_id = arg_model_id->get_string();
    ObString query = arg_query->get_string();
    ObArray<ObString> header_array;
    ObJsonArray *document_array = nullptr;
    ObJsonArray *request_document_array = nullptr;
    ObJsonArray *result_array = nullptr;
    ObIJsonBase *j_base = nullptr;
    const int64_t batch_size = 20;
    bool is_null_result = false;
    const bool with_doc_key = OB_NOT_NULL(arg_doc_key) && !arg_doc_key->is_null();
    if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, temp_allocator, DOCUMENTS_IDX,
          j_base, is_null_result))) {
    } else if (OB_ISNULL(j_base)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (j_base->json_type() != ObJsonNodeType::J_ARRAY) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is not array");
    } else if (OB_ISNULL(document_array = static_cast<ObJsonArray *>(j_base))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (document_array->element_count() == 0) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is empty");
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < document_array->element_count(); i++) {
        ObJsonNode *doc_node = document_array->get_value(i);
        if (OB_ISNULL(doc_node)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (with_doc_key && doc_node->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not object");
        } else if (!with_doc_key && doc_node->json_type() != ObJsonNodeType::J_STRING) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not string");
        }
      }
    }
    
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else if (OB_ISNULL(info)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(ObAIFuncUtils::check_info_type_rerank(info))) {
    } else if (OB_FAIL(ObAIFuncUtils::get_header(
                   temp_allocator, *info, *endpoint_info, header_array))) {
    } else if (with_doc_key) {
      ObString doc_key = arg_doc_key->get_string();
      if (OB_FAIL(get_doc_array_from_documents_array_with_key(
              temp_allocator, document_array, doc_key, request_document_array))) {
      }
    } else {
      request_document_array = document_array;
    }

    struct ScoredResult {
      ObJsonNode *node_;
      double score_;
    };
    ObArray<ScoredResult> all_results;
    if (OB_FAIL(ret)) {
    } else {
      for (int64_t start_idx = 0;
           OB_SUCC(ret) && start_idx < request_document_array->element_count();
           start_idx += batch_size) {
        const int64_t end_idx =
            MIN(start_idx + batch_size, request_document_array->element_count());
        ObJsonArray *batch_document_array = nullptr;
        ObJsonArray *batch_result_array = nullptr;
        ObJsonArray *mapped_result_array = nullptr;
        if (OB_FAIL(construct_batch_document_array(
                temp_allocator, request_document_array, start_idx, end_idx,
                batch_document_array))) {
        } else if (OB_FAIL(inner_eval_ai_rerank(
                       temp_allocator, *info, *endpoint_info, header_array, query,
                       batch_document_array, batch_result_array))) {
        } else if (OB_FAIL(map_rerank_batch(
                       temp_allocator, document_array, batch_result_array,
                       start_idx, end_idx - start_idx, with_doc_key,
                       mapped_result_array))) {
        } else {
          for (int64_t result_idx = 0;
               OB_SUCC(ret) && result_idx < mapped_result_array->element_count();
               ++result_idx) {
            ObJsonNode *result_node = mapped_result_array->get_value(result_idx);
            double score = 0.0;
            ScoredResult scored_result;
            if (OB_ISNULL(result_node)) {
              ret = OB_ERR_UNEXPECTED;
            } else if (OB_FAIL(get_rerank_score(result_node, score))) {
            } else {
              scored_result.node_ = result_node;
              scored_result.score_ = score;
              if (OB_FAIL(all_results.push_back(scored_result))) {
              }
            }
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      std::stable_sort(
          all_results.begin(), all_results.end(),
          [](const ScoredResult &left, const ScoredResult &right) {
            return left.score_ > right.score_;
          });
      if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(
              temp_allocator, result_array))) {
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < all_results.count(); ++i) {
        if (OB_FAIL(result_array->append(all_results.at(i).node_))) {
        }
      }
    }
    if (OB_SUCC(ret)) {
      ObString raw_str;
      if (OB_FAIL(result_array->get_raw_binary(raw_str, &temp_allocator))) {
      } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(
                     expr, ctx, res, raw_str))) {
      }
    }
  }
  return ret;
}

int ObExprAIRerank::get_doc_array_from_documents_array_with_key(ObIAllocator &allocator, ObJsonArray *document_object_array, ObString &doc_key, ObJsonArray *&doc_array)
{
  INIT_SUCC(ret);
  if (OB_ISNULL(document_object_array)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (doc_key.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonArray *doc_array_obj = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, doc_array_obj))) {
    } else {
      int64_t count = document_object_array->element_count();
      ObJsonNode *doc_node = nullptr;
      ObJsonNode *doc_value_node = nullptr;
      ObJsonObject *doc_obj = nullptr;
      for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
        doc_node = document_object_array->get_value(i);
        if (OB_ISNULL(doc_node)) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, document_array element is null");
        } else if (doc_node->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not object");
        } else {
          doc_obj = static_cast<ObJsonObject *>(doc_node);
          doc_value_node = doc_obj->get_value(doc_key);
          if (OB_ISNULL(doc_value_node)) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is null");
          } else if (doc_value_node->json_type() != ObJsonNodeType::J_STRING) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is not string");
          } else if (OB_FAIL(doc_array_obj->append(doc_value_node))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      doc_array = doc_array_obj;
    }
  }
  return ret;
}

int ObExprAIRerank::map_rerank_batch(ObIAllocator &allocator,
                                     ObJsonArray *document_array,
                                     ObJsonArray *model_result_array,
                                     int64_t start_idx,
                                     int64_t batch_count,
                                     bool with_doc_key,
                                     ObJsonArray *&mapped_result_array)
{
  INIT_SUCC(ret);
  struct ScoredResult {
    ObJsonObject *object_;
    double score_;
  };
  ObJsonArray *mapped_array = nullptr;
  ObArray<ScoredResult> ordered_results;
  char *index_seen = nullptr;
  if (OB_ISNULL(document_array) || OB_ISNULL(model_result_array)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (batch_count <= 0 || batch_count > 20) {
    ret = OB_ERR_UNEXPECTED;
  } else if (start_idx < 0
             || start_idx + batch_count > document_array->element_count()) {
    ret = OB_ERR_UNEXPECTED;
  } else if (model_result_array->element_count() != batch_count) {
    ret = OB_INVALID_DATA;
  } else if (OB_ISNULL(index_seen =
                          static_cast<char *>(allocator.alloc(batch_count)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, mapped_array))) {
  } else {
    MEMSET(index_seen, 0, batch_count);
    for (int64_t i = 0;
         OB_SUCC(ret) && i < model_result_array->element_count();
         ++i) {
      ObJsonNode *result_node = model_result_array->get_value(i);
      ObJsonObject *model_result = nullptr;
      ObJsonNode *index_node = nullptr;
      ObJsonNode *score_node = nullptr;
      int64_t local_index = 0;
      double score = 0.0;
      if (OB_ISNULL(result_node)
          || result_node->json_type() != ObJsonNodeType::J_OBJECT) {
        ret = OB_INVALID_DATA;
      } else {
        model_result = static_cast<ObJsonObject *>(result_node);
        index_node = model_result->get_value(INDEX_KEY);
        score_node = model_result->get_value(SCORE_KEY);
        if (OB_ISNULL(index_node) || OB_ISNULL(score_node)) {
          ret = OB_INVALID_DATA;
        } else if (ObJsonNodeType::J_INT == index_node->json_type()) {
          local_index = static_cast<ObJsonInt *>(index_node)->value();
        } else if (ObJsonNodeType::J_UINT == index_node->json_type()) {
          const uint64_t uint_index =
              static_cast<ObJsonUint *>(index_node)->value();
          if (uint_index > static_cast<uint64_t>(INT64_MAX)) {
            ret = OB_INVALID_DATA;
          } else {
            local_index = static_cast<int64_t>(uint_index);
          }
        } else {
          ret = OB_INVALID_DATA;
        }
      }
      if (OB_FAIL(ret)) {
      } else if (local_index < 0 || local_index >= batch_count
                 || index_seen[local_index]) {
        ret = OB_INVALID_DATA;
      } else if (OB_FAIL(get_rerank_score(model_result, score))) {
        ret = OB_INVALID_DATA;
      } else {
        index_seen[local_index] = 1;
        ObJsonObject *output_object = nullptr;
        if (with_doc_key) {
          ObJsonNode *source_node = document_array->get_value(start_idx + local_index);
          if (OB_ISNULL(source_node)
              || source_node->json_type() != ObJsonNodeType::J_OBJECT) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_ISNULL(output_object =
                                   static_cast<ObJsonObject *>(
                                       source_node->clone(&allocator, true)))) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            if (OB_NOT_NULL(output_object->get_value(MODEL_SCORE_KEY))
                && OB_FAIL(output_object->remove(MODEL_SCORE_KEY))) {
            } else if (OB_FAIL(output_object->add(MODEL_SCORE_KEY, score_node))) {
            }
          }
        } else if (OB_ISNULL(output_object =
                                 static_cast<ObJsonObject *>(
                                     model_result->clone(&allocator, true)))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          const int64_t global_index = start_idx + local_index;
          ObJsonInt *global_index_node = nullptr;
          if (OB_NOT_NULL(output_object->get_value(INDEX_KEY))
              && OB_FAIL(output_object->remove(INDEX_KEY))) {
          } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(
                         allocator, global_index, global_index_node))) {
          } else if (OB_FAIL(output_object->add(INDEX_KEY, global_index_node))) {
          }
        }
        if (OB_SUCC(ret)) {
          ScoredResult ordered_result;
          ordered_result.object_ = output_object;
          ordered_result.score_ = score;
          if (OB_FAIL(ordered_results.push_back(ordered_result))) {
          }
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    for (int64_t i = 1; i < ordered_results.count(); ++i) {
      ScoredResult current = ordered_results.at(i);
      int64_t position = i;
      while (position > 0
             && ordered_results.at(position - 1).score_ < current.score_) {
        ordered_results.at(position) = ordered_results.at(position - 1);
        --position;
      }
      ordered_results.at(position) = current;
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < ordered_results.count(); ++i) {
      if (OB_FAIL(mapped_array->append(ordered_results.at(i).object_))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    mapped_result_array = mapped_array;
  }
  return ret;
}

int ObExprAIRerank::construct_batch_document_array(ObIAllocator &allocator, ObJsonArray *document_array, int64_t start_idx, int64_t end_idx, ObJsonArray *&batch_document_array)
{
  INIT_SUCC(ret);
  ObJsonArray *result_array = nullptr;
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, result_array))) {
  } else {
    for (int64_t i = start_idx; i < end_idx; i++) {
      if (OB_FAIL(result_array->append(document_array->get_value(i)))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    batch_document_array = result_array;
  }
  return ret;
}

int ObExprAIRerank::inner_eval_ai_rerank(ObIAllocator &allocator, 
                                          const ObAIFuncExprInfo &info, 
                                          const share::ObAiModelEndpointInfo &endpoint_info,
                                          ObArray<ObString> &header_array, 
                                          ObString &query, 
                                          ObJsonArray *document_array, 
                                          ObJsonArray *&result_array)
{
  INIT_SUCC(ret);
  ObJsonArray *res = nullptr;
  ObJsonObject *config_json = nullptr;
  ObJsonObject *body = nullptr;
  ObJsonObject *http_response = nullptr;
  ObIJsonBase *response = nullptr;
  ObAIFuncClient ai_client;
  if (OB_FAIL(ObAIFuncUtils::get_rerank_body(allocator, info, endpoint_info, query, document_array, config_json, body))) {
  } else if (OB_FAIL(ai_client.send_post(allocator, endpoint_info.get_url(), header_array, body, http_response))) {
  } else if (OB_FAIL(ObAIFuncUtils::parse_rerank_output(allocator, endpoint_info, http_response, response))) {
  } else if (response->json_type() != ObJsonNodeType::J_ARRAY) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(res = static_cast<ObJsonArray *>(response))) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    result_array = res;
  }
  return ret;
}

int ObExprAIRerank::get_rerank_score(const ObJsonNode *result, double &score)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(result)
      || result->json_type() != ObJsonNodeType::J_OBJECT) {
    ret = OB_INVALID_DATA;
  } else {
    const ObJsonObject *result_object =
        static_cast<const ObJsonObject *>(result);
    ObJsonNode *score_node = result_object->get_value(SCORE_KEY);
    if (OB_ISNULL(score_node)) {
      score_node = result_object->get_value(MODEL_SCORE_KEY);
    }
    if (OB_ISNULL(score_node)) {
      ret = OB_INVALID_DATA;
    } else {
      const ObJsonNodeType score_type = score_node->json_type();
      if (ObJsonNodeType::J_INT != score_type
          && ObJsonNodeType::J_UINT != score_type
          && ObJsonNodeType::J_DOUBLE != score_type
          && ObJsonNodeType::J_DECIMAL != score_type) {
        ret = OB_INVALID_DATA;
      } else if (OB_FAIL(score_node->to_double(score))
                 || !std::isfinite(score)) {
        ret = OB_INVALID_DATA;
      }
    }
  }
  return ret;
}

int ObExprAIRerank::construct_config_json(ObIAllocator &allocator, int64_t top_n, int64_t return_doc, ObJsonObject *&config_json)
{
  INIT_SUCC(ret);
  ObJsonObject *config_obj = nullptr;
  ObJsonInt *top_n_obj = nullptr;
  ObJsonBoolean *return_doc_obj = nullptr;
  bool return_doc_value = true;
  if (return_doc <= 0) {
    return_doc_value = false;
  }
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config_obj))) {
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_boolean(allocator, return_doc_value, return_doc_obj))) {
  } else if (OB_FAIL(config_obj->add("return_documents", return_doc_obj))) {
  } else if (top_n > 0) {
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, top_n, top_n_obj))) {
    } else if (OB_FAIL(config_obj->add("top_n", top_n_obj))) {
    }
  }
  if (OB_SUCC(ret)) {
    config_json = config_obj;
  }
  return ret;
}

int ObExprAIRerank::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                        const ObRawExpr &raw_expr,
                        ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   bool got_data = false;
  //   ObObj const_data;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else {
  //     ObAIFuncExprInfo *info = OB_NEWx(ObAIFuncExprInfo, allocator, *allocator, T_FUN_SYS_AI_RERANK);
  //     if (OB_ISNULL(info)) {
  //       ret = OB_ALLOCATE_MEMORY_FAILED;
  //       LOG_WARN("failed to allocate memory for ai rerank info", K(ret));
  //     } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //       LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //     } else {
  //       rt_expr.extra_info_ = info;
  //     }
  //   }
  // }
  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIRerank::eval_ai_rerank;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
