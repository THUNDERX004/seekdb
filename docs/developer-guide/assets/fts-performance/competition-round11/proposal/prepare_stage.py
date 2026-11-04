"""Prepare auxiliary files only; never write to SOURCE. Run once, before review."""
from datetime import datetime, timezone
import difflib
import hashlib
import json
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
SOURCE = Path(r"D:\Projects\OceanBase\seekdb")
BASELINE = "5bce9e06d1281347ad61344e4b01fab9158c58a2"
TARGETS = (
    "src/sql/code_generator/ob_static_engine_cg.cpp",
    "src/storage/retrieval/ob_i_sparse_retrieval_iter.h",
    "src/observer/composition/retrieval/ob_das_legacy_tr_merge_iter.cpp",
    "src/storage/retrieval/ob_sparse_daat_iter.cpp",
)
CONTEXT = (
    "src/storage/fts/ik/ob_ik_char_util.h",
    "src/storage/fts/ik/ob_ik_processor.cpp",
    "src/storage/fts/ik/ob_ik_processor.h",
    "src/storage/retrieval/ob_text_retrieval_token_iter.cpp",
    "src/storage/retrieval/ob_text_retrieval_token_iter.h",
    # Git reports a raw line-ending-only modification here; not a payload.
    "src/data_plane/api/data_plane/fts/ob_fts_struct.h",
)


def git(*args):
    return subprocess.check_output(["git", "-C", str(SOURCE), *args]).decode().strip()


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


def blob(raw):
    return hashlib.sha1(b"blob " + str(len(raw)).encode() + b"\0" + raw).hexdigest()


def lf(raw):
    return raw.replace(b"\r\n", b"\n")


def replace_once(text, before, after):
    if text.count(before) != 1:
        raise RuntimeError("Source anchor is not unique: " + before[:100])
    return text.replace(before, after, 1)


def change(path, text):
    if path == TARGETS[0]:
        anchor = "\n  if (log_op_def::LOG_SET == op.get_type()\n"
        addition = '''
  // Only a complete, value-free IR root may discard its external ID output.
  // Keep the child scan's ID expression and metadata for retrieval/merging.
  if (OB_SUCC(ret) && PHY_TABLE_SCAN == spec.type_
      && log_op_def::LOG_TABLE_SCAN == op.get_type()) {
    ObLogTableScan &log_tsc = static_cast<ObLogTableScan &>(op);
    ObTableScanSpec &tsc_spec = static_cast<ObTableScanSpec &>(spec);
    ObTableScanCtDef &tsc_ctdef = tsc_spec.tsc_ctdef_;
    ObDASScanCtDef &scan_ctdef = tsc_ctdef.scan_ctdef_;
    ObDASBaseCtDef *root = tsc_ctdef.attach_spec_.attach_ctdef_;
    if (spec.output_.empty() && spec.filters_.empty()
        && log_tsc.get_access_exprs().empty()
        && !log_tsc.get_index_back() && !tsc_spec.is_index_global_
        && !tsc_ctdef.use_index_merge_ && OB_ISNULL(tsc_ctdef.lookup_ctdef_)
        && !tsc_ctdef.is_das_keep_order_ && !tsc_ctdef.ordering_used_by_parent_
        && OB_ISNULL(scan_ctdef.group_id_expr_)
        && OB_ISNULL(scan_ctdef.trans_info_expr_)
        && OB_ISNULL(scan_ctdef.pd_expr_spec_.trans_info_expr_)
        && scan_ctdef.pd_expr_spec_.pd_storage_aggregate_output_.empty()
        && OB_ISNULL(tsc_ctdef.calc_part_id_expr_)
        && tsc_ctdef.global_index_rowkey_exprs_.empty()
        && OB_ISNULL(tsc_spec.pdml_partition_id_)
        && !tsc_spec.report_col_checksum_ && !tsc_spec.can_be_paused_
        && !tsc_spec.is_scan_resumable_
        && OB_NOT_NULL(root) && DAS_OP_IR_SCAN == root->op_type_
        && 1 == root->children_cnt_ && OB_NOT_NULL(root->children_)
        && root->children_[0] == &scan_ctdef) {
      ObDASIRScanCtDef &ir_ctdef = static_cast<ObDASIRScanCtDef &>(*root);
      if (ObMatchAgainstMode::NATURAL_LANGUAGE_MODE == ir_ctdef.mode_flag_
          && !ir_ctdef.need_calc_relevance()
          && !ir_ctdef.need_proj_relevance_score()
          && OB_ISNULL(ir_ctdef.match_filter_) && !ir_ctdef.has_pushdown_topk()
          && OB_NOT_NULL(ir_ctdef.inv_scan_domain_id_col_)
          && 1 == ir_ctdef.result_output_.count()
          && ir_ctdef.result_output_.at(0) == ir_ctdef.inv_scan_domain_id_col_) {
        // An empty existing output array carries the dependency contract; no
        // new serialized flag, expression generation, or ID-meta removal.
        ir_ctdef.result_output_.clear();
      }
    }
  }
'''
        return replace_once(text, anchor, addition + anchor)
    if path == TARGETS[1]:
        text = replace_once(text, "      max_batch_size_(1)\n", "      max_batch_size_(1),\n      materialize_results_(true)\n")
        text = replace_once(text, "      K_(topk_limit), K_(max_batch_size));", "      K_(topk_limit), K_(max_batch_size), K_(materialize_results));")
        return replace_once(text, "  int64_t max_batch_size_;\n};", "  int64_t max_batch_size_;\n  // Runtime only. The ID expression remains mandatory for internal metadata.\n  bool materialize_results_;\n};")
    if path == TARGETS[2]:
        anchor = "  sr_iter_param_.topk_limit_ = topk_limit_;\n"
        return replace_once(text, anchor, anchor + '''  // CG clears only a value-free top-level IR root's external output array.
  // Other algorithms and functional lookup retain their existing projection.
  sr_iter_param_.materialize_results_ = !(daat_mode_ && !topk_mode_
      && !function_lookup_mode_ && ir_ctdef_->result_output_.empty()
      && ObMatchAgainstMode::NATURAL_LANGUAGE_MODE == ir_ctdef_->mode_flag_
      && !ir_ctdef_->need_calc_relevance()
      && !ir_ctdef_->need_proj_relevance_score()
      && OB_ISNULL(ir_ctdef_->match_filter_) && !ir_ctdef_->has_pushdown_topk());
''')
    if path == TARGETS[3]:
        text = replace_once(text, '''    buffered_domain_ids_[count].from_datum(id_datum);
    buffered_relevances_[count] = relevance;
''', '''    if (iter_param_->materialize_results_) {
      buffered_domain_ids_[count].from_datum(id_datum);
      buffered_relevances_[count] = relevance;
    }
''')
        anchor = '''  ObEvalCtx::BatchInfoScopeGuard guard(*eval_ctx);

  if (eval_ctx->is_vectorized()) {
    sql::ObBitVector &id_evaluated_flags'''
        return replace_once(text, anchor, '''  ObEvalCtx::BatchInfoScopeGuard guard(*eval_ctx);

  if (!iter_param_->materialize_results_) {
    // Preserve the scalar cardinality contract even without datum writes.
    if (!eval_ctx->is_vectorized() && OB_UNLIKELY(1 != count)) {
      ret = OB_ERR_UNEXPECTED;
    }
  } else if (eval_ctx->is_vectorized()) {
    sql::ObBitVector &id_evaluated_flags''')
    raise AssertionError(path)


def snapshot(path, prefix):
    raw = (SOURCE / path).read_bytes()
    destination = HERE / prefix / path
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("xb") as output:
        output.write(raw)
    return raw, {
        "path": path,
        "original_git_blob": git("rev-parse", "HEAD:" + path),
        "original_raw_git_blob": blob(raw),
        "original_content_sha256": digest(raw),
        "original_content_bytes": len(raw),
        "original_lf_sha256": digest(lf(raw)),
        "original_lf_git_blob": blob(lf(raw)),
        "snapshot": destination.relative_to(HERE).as_posix(),
    }


def main():
    if (HERE / "manifest.json").exists():
        raise SystemExit("Refusing to replace an existing proposal")
    head = git("rev-parse", "HEAD")
    records, patches, context_records = [], [], []
    for path in TARGETS:
        original, record = snapshot(path, "original")
        text = lf(original).decode("utf-8")
        updated_text = change(path, text)
        updated = updated_text.encode("utf-8")
        if b"\r\n" in original:
            if original.count(b"\r\n") != original.count(b"\n"):
                raise RuntimeError("Mixed source line endings: " + path)
            updated = updated.replace(b"\n", b"\r\n")
        destination = HERE / path
        destination.parent.mkdir(parents=True, exist_ok=True)
        with destination.open("xb") as output:
            output.write(updated)
        record.update(new_sha256=digest(updated), new_bytes=len(updated),
                      new_raw_git_blob=blob(updated), new_lf_git_blob=blob(lf(updated)),
                      line_endings="CRLF" if b"\r\n" in original else "LF")
        records.append(record)
        patches.append("diff --git a/" + path + " b/" + path + "\n")
        patches.extend(difflib.unified_diff(text.splitlines(True), updated_text.splitlines(True),
                                          fromfile="a/" + path, tofile="b/" + path))
    for path in CONTEXT:
        _, record = snapshot(path, "context")
        context_records.append(record)
    if git("rev-parse", "HEAD") != head:
        raise RuntimeError("HEAD changed while capturing")
    for record in records + context_records:
        if digest((SOURCE / record["path"]).read_bytes()) != record["original_content_sha256"]:
            raise RuntimeError("Source changed while capturing: " + record["path"])
    (HERE / "source.patch").write_text("".join(patches), encoding="utf-8", newline="\n")
    manifest = {
        "schema_version": 1,
        "purpose": "round11 query-result materialization; auxiliary staging only",
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "source_root": str(SOURCE), "source_head": head,
        "source_branch": git("rev-parse", "--abbrev-ref", "HEAD"),
        "official_baseline": BASELINE,
        "source_files": records, "context_only_files": context_records,
        "source_write_set": list(TARGETS),
        "fixture_generated": False,
        "validation": "pending static audit; no build, SQL, perf or native execution",
    }
    (HERE / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print("STAGED", len(records), "source files; HEAD", head)


if __name__ == "__main__":
    main()
