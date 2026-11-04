/* Copyright (c) 2026. Licensed under the Apache License, Version 2.0. */
// Extended native sidecar only; frozen 152-case source is a separate file.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include "lib/allocator/page_arena.h"
#include "lib/rc/context.h"
#include "common/datum/ob_datum.h"
#include "storage/blocksstable/encoding/ob_bit_stream.h"
#include "storage/blocksstable/encoding/ob_row_index.h"
#include "storage/blocksstable/encoding/ob_string_diff_decoder.h"
using namespace oceanbase;
using namespace oceanbase::common;
using namespace oceanbase::blocksstable;
namespace {
int cases = 0;
void need(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
struct Result {
  int ret; uint32_t flags, len; std::string bytes;
  bool operator==(const Result &r) const
  { return ret == r.ret && flags == r.flags && len == r.len && bytes == r.bytes; }
};
Result owned(int ret, const ObDatum &d)
{
  return {ret, d.pack_, d.len_, ret == OB_SUCCESS && !d.is_null()
      ? std::string(d.ptr_, d.len_) : std::string()};
}
void emit(const char *kind, int slot, int row, const Result &r)
{
  std::printf("T\t%d\t%s\t%d\t%d\t%d\t%08x\t%u\t",
      cases, kind, slot, row, r.ret, unsigned(r.flags), unsigned(r.len));
  for (unsigned char c : r.bytes) std::printf("%02x", unsigned(c));
  std::putchar('\n');
}
class GuardArena final : public ObIAllocator {
public:
  struct Block { unsigned char *raw; int64_t size; };
  ObArenaAllocator arena; std::vector<Block> blocks; bool fail = false;
  int calls = 0; void *last = nullptr;
  void *alloc(int64_t n) override
  {
    ++calls; last = nullptr;
    if (fail) return nullptr;
    if (n <= 0) return last = arena.alloc(n); // Preserve the real zero-size policy.
    auto *p = static_cast<unsigned char *>(arena.alloc(n + 32));
    if (p) { std::memset(p, 0xd3, 16); std::memset(p + 16, 0xa5, n);
      std::memset(p + 16 + n, 0x6b, 16); blocks.push_back({p, n}); last = p + 16; }
    return last;
  }
  void *alloc(int64_t n, const ObMemAttr &) override { return alloc(n); }
  void free(void *) override {} // Actual arena lifetime owns all allocations.
  void check() const
  {
    for (const auto &b : blocks) for (int i = 0; i < 16; ++i)
      need(b.raw[i] == 0xd3 && b.raw[16 + b.size + i] == 0x6b, "allocation canary");
  }
};
struct Fixture {
  static constexpr int N = 67;
  ObMicroBlockHeader micro; ObColumnHeader col; ObStringDiffDecoder decoder;
  ObFixRowIndex fix_index; ObVarRowIndex var_index;
  std::vector<unsigned char> meta, rows; std::array<std::string, N> source;
  std::array<bool, N> nulls{}; int size, stride, desc_count; bool variable;
  Fixture(int length, bool hex, int layout, int null_mode, bool var)
      : size(length), stride(std::max(length, 128)), variable(var)
  {
    std::vector<uint8_t> diff(length);
    for (int j = 0; j < length; ++j) diff[j] = layout == 0 ? j >= length / 2
        : layout == 1 ? j == length / 2 : layout == 2 ? (j % 2 != 0 || length == 1)
        : (j == 0 || j == length - 1);
    std::vector<ObStringDiffHeader::DiffDesc> desc;
    int common = 0, changes = 0;
    for (int j = 0; j < length;) {
      int run = 1; while (j + run < length && run < 127 && diff[j + run] == diff[j]) ++run;
      ObStringDiffHeader::DiffDesc d{}; d.diff_ = diff[j]; d.count_ = run;
      desc.push_back(d); j += run;
    }
    for (int j = 0; j < length; ++j) diff[j] ? ++changes : ++common;
    need(changes > 0 && desc.size() <= 255, "invalid producer descriptor plan");
    desc_count = static_cast<int>(desc.size());
    static const unsigned char alphabet[] = "0123456789abCD_-";
    for (int r = 0; r < N; ++r) {
      nulls[r] = null_mode == 1 ? r == 7 : null_mode == 2 ? r % 4 == 0 : null_mode == 3;
      source[r].resize(length);
      for (int j = 0; j < length; ++j) source[r][j] = static_cast<char>(diff[j]
          ? alphabet[(r * 3 + j * 5) % 16] : (j % 7 == 0 ? 0 : 0x80 + j % 75));
    }
    const int payload = hex ? (changes + 1) / 2 : changes;
    const int meta_size = sizeof(ObStringDiffHeader) + desc_count + (hex ? 16 : 0) + common;
    const int bitmap = null_mode == 0 ? 0 : (N + 7) / 8;
    meta.resize(meta_size + (var ? 0 : bitmap + N * payload) + 32, 0);
    auto *h = new (meta.data()) ObStringDiffHeader;
    h->string_size_ = length; h->diff_desc_cnt_ = desc_count; h->hex_char_array_size_ = hex ? 16 : 0;
    h->offset_ = var && null_mode != 0 ? 1 : 0; h->length_ = var ? 0 : payload;
    std::copy(desc.begin(), desc.end(), h->diff_descs_);
    ObHexStringMap map;
    if (hex) { for (int i = 0; i < 16; ++i) map.mark(alphabet[i]); map.build_index(h->hex_char_array()); }
    int pos = 0;
    for (int j = 0; j < length; ++j) if (!diff[j]) h->common_data()[pos++] = source[0][j];
    col.type_ = ObColumnHeader::STRING_DIFF; col.obj_type_ = ObVarcharType; col.length_ = meta_size;
    if (var) col.set_last_var_field_attr(); else col.set_fix_lenght_attr();
    if (null_mode != 0) col.set_has_extend_value_attr();
    micro.row_count_ = N; micro.column_count_ = 1; micro.rowkey_column_count_ = 0;
    micro.var_column_count_ = var ? 1 : 0; micro.extend_value_bit_ = null_mode == 0 ? 0 : 1;
    std::vector<uint16_t> offsets;
    for (int r = 0; r < N; ++r) {
      std::vector<unsigned char> cell(payload + 16, 0);
      ObHexStringPacker packer(map, cell.data()); int k = 0;
      for (int j = 0; j < length; ++j) if (diff[j]) {
        if (hex) packer.pack(static_cast<unsigned char>(source[r][j]));
        else cell[k++] = static_cast<unsigned char>(source[r][j]);
      }
      if (var) {
        offsets.push_back(static_cast<uint16_t>(rows.size()));
        if (null_mode != 0) rows.push_back(nulls[r] ? STORED_NULL : STORED_NOT_EXT);
        if (!nulls[r]) rows.insert(rows.end(), cell.begin(), cell.begin() + payload);
      } else {
        if (bitmap) { ObBitStream bs(meta.data() + meta_size, bitmap);
          need(bs.set(r, 1, uint64_t(nulls[r] ? STORED_NULL : STORED_NOT_EXT)) == OB_SUCCESS, "bitmap pack"); }
        std::memcpy(meta.data() + meta_size + bitmap + r * payload, cell.data(), payload);
      }
    }
    if (var) {
      offsets.push_back(static_cast<uint16_t>(rows.size()));
      const auto *p = reinterpret_cast<const unsigned char *>(offsets.data());
      rows.insert(rows.end(), p, p + offsets.size() * sizeof(uint16_t));
      const int len = static_cast<int>(rows.size()); rows.resize(len + 32, 0);
      need(var_index.init(reinterpret_cast<const char *>(rows.data()), len, N, 2) == OB_SUCCESS, "var row index");
    } else need(fix_index.init(reinterpret_cast<const char *>(meta.data() + meta_size + bitmap),
        N * payload, N) == OB_SUCCESS, "fixed row index");
    need(decoder.init(micro, col, reinterpret_cast<const char *>(meta.data())) == OB_SUCCESS, "decoder init");
  }
  const ObIRowIndex *index() const { return variable ? static_cast<const ObIRowIndex *>(&var_index) : &fix_index; }
  ObColumnDecoderCtx context(ObIAllocator &a) const
  { ObColumnDecoderCtx c; ObObjMeta m; m.set_varchar(); m.set_collation_type(CS_TYPE_BINARY);
    c.fill(m, &micro, &col, &a, 0); return c; }
  Result oracle(int r) const
  { ObDatum d; if (nulls[r]) d.set_null(); else d.set_string(source[r].data(), size); return owned(OB_SUCCESS, d); }
};
class ContextScope final {
public:
  ContextScope() { lib::ContextParam p; p.set_mem_attr("R19StringDiff", ObCtxIds::DEFAULT_CTX_ID)
      .set_properties(lib::USE_TL_PAGE_OPTIONAL);
    need(CURRENT_CONTEXT->CREATE_CONTEXT(context_, p) == OB_SUCCESS && context_ != nullptr, "CREATE_CONTEXT");
    flow_.emplace(context_); if (flow_->init() != OB_SUCCESS) {
      flow_.reset(); DESTROY_CONTEXT(context_); context_ = nullptr; need(false, "Flow::init"); } }
  ~ContextScope() { if (flow_) { flow_->deinit(); flow_.reset(); }
    if (context_ != nullptr) DESTROY_CONTEXT(context_); }
private:
  lib::MemoryContext context_ = nullptr; std::optional<lib::Flow> flow_;
};
void run(int length, bool hex, int layout, int null_mode, int cap, bool var = false,
    bool fail = false, bool distinct_pair = false)
{
  ++cases; Fixture f(length, hex, layout, null_mode, var); GuardArena a, scalar;
  a.fail = scalar.fail = fail; auto ctx = f.context(a); auto sc = f.context(scalar);
  std::vector<int32_t> ids(std::max(cap, 1));
  for (int i = 0; i < cap; ++i) ids[i] = i == 0 || i % 5 == 0 ? 7 : 66 - (11 * i) % Fixture::N;
  if (distinct_pair && cap == 2) {
    ids[1] = 44; need(f.source[ids[0]] != f.source[ids[1]], "non-distinct two-row source");
  }
  std::vector<ObDatum> d(std::max(cap, 1)), next(std::max(cap, 1));
  std::printf("CASE\t%d\tlen=%d\thex=%d\tdesc=%d\tcap=%d\tnull=%d\tvar=%d\tfail=%d\n",
      cases, length, int(hex), f.desc_count, cap, null_mode, int(var), int(fail));
  const auto meta_before = f.meta, rows_before = f.rows;
  int ret = f.decoder.batch_decode(ctx, f.index(), ids.data(), nullptr, cap, d.data());
  std::printf("B\t%d\tfirst\t%d\n", cases, ret);
  need(a.calls == 1, "batch allocation count");
  const int expected = fail || (cap == 0 && !a.last) ? OB_ALLOCATE_MEMORY_FAILED : OB_SUCCESS;
  need(ret == expected, "unexpected batch return");
  std::vector<Result> saved;
  for (int i = 0; i < cap; ++i) {
    auto b = owned(ret, d[i]); emit("batch", i, ids[i], b); saved.push_back(b);
    const char *row = nullptr; int64_t row_len = 0; ObDatum one;
    need(f.index()->get(ids[i], row, row_len) == OB_SUCCESS, "scalar row locate");
    ObBitStream bs(reinterpret_cast<unsigned char *>(const_cast<char *>(row)), row_len);
    const int sr = f.decoder.decode(sc, one, ids[i], bs, row, row_len);
    const auto s = owned(sr, one); emit("scalar", i, ids[i], s);
    if (!fail) need(b == s && b == f.oracle(ids[i]), "scalar/batch/source mismatch");
    else need(sr == (f.nulls[ids[i]] ? OB_SUCCESS : OB_ALLOCATE_MEMORY_FAILED)
        && b.flags == s.flags && b.len == s.len, "allocation-failure flags/return");
  }
  if (!fail && cap > 0) {
    need(a.blocks.size() == 1 && a.blocks[0].size == int64_t(f.stride) * cap, "batch buffer shape");
    for (int i = 0; i < cap; ++i) if (!d[i].is_null())
      need(d[i].ptr_ == reinterpret_cast<char *>(a.blocks[0].raw + 16 + i * f.stride), "owned row slot");
    std::reverse(ids.begin(), ids.begin() + cap);
    const int nr = f.decoder.batch_decode(ctx, f.index(), ids.data(), nullptr, cap, next.data());
    std::printf("B\t%d\tsecond\t%d\n", cases, nr); need(nr == OB_SUCCESS, "second batch");
    std::vector<Result> second;
    for (int i = 0; i < cap; ++i) {
      second.push_back(owned(nr, next[i])); emit("second", i, ids[i], second.back());
      need(second.back() == f.oracle(ids[i]) && owned(ret, d[i]) == saved[i], "other batch clobbered");
    }
    for (const auto &block : a.blocks) for (int i = 0; i < cap; ++i)
      for (int j = length; j < f.stride; ++j) need(block.raw[16 + i * f.stride + j] == 0xa5, "row slack canary");
    need(f.meta == meta_before && f.rows == rows_before, "encoded input mutated");
    std::fill(f.meta.begin(), f.meta.end(), 0xec); std::fill(f.rows.begin(), f.rows.end(), 0x9b);
    for (int i = 0; i < cap; ++i) need(owned(ret, d[i]) == saved[i]
        && owned(nr, next[i]) == second[i], "result aliases encoded input");
    for (int i = 0; i < cap; ++i) if (!d[i].is_null()) {
      auto *p = const_cast<char *>(d[i].ptr_); const char old = *p; *p ^= 0x5a;
      for (int j = 0; j < cap; ++j) need(owned(nr, next[j]) == second[j]
          && (j == i || owned(ret, d[j]) == saved[j]), "result buffers overlap");
      *p = old; break;
    }
  }
  a.check(); scalar.check();
}
class FailingRowIndex final : public ObIRowIndex {
public:
  FailingRowIndex(const ObIRowIndex &source, int fail_on) : source_(source), fail_on_(fail_on) {}
  int get(const int64_t row_id, const char *&data, int64_t &len) const override
  { return gets++ == fail_on_ ? OB_ERR_UNEXPECTED : source_.get(row_id, data, len); }
  int batch_get(const int32_t *ids, const int64_t cap, const bool ext,
      const char **data, ObDatum *d) const override
  { ++batches; return source_.batch_get(ids, cap, ext, data, d); }
  int batch_get(const int32_t *ids, const int64_t cap, const bool ext,
      const char **data, uint32_t *len) const override
  { ++batches; return source_.batch_get(ids, cap, ext, data, len); }
  mutable int gets = 0, batches = 0;
private:
  const ObIRowIndex &source_; int fail_on_;
};
void run_partial_failure(int length, bool hex)
{
  ++cases; constexpr int cap = 7, prefix = 3;
  Fixture f(length, hex, 2, 0, true); GuardArena a, scalar;
  auto ctx = f.context(a); auto sc = f.context(scalar);
  need(!ctx.has_extend_value() && !ctx.is_fix_length(), "failure must follow published prefix");
  const std::array<int32_t, cap> ids{{7, 44, 33, 22, 7, 0, 55}};
  FailingRowIndex index(*f.index(), prefix);
  std::array<ObDatum, cap> d, before;
  std::array<std::array<char, 16>, cap> sentinels{};
  std::array<Result, cap> untouched, saved;
  for (int i = 0; i < cap; ++i) {
    for (int j = 0; j < 16; ++j) sentinels[i][j] = static_cast<char>(0xb0 + i + j);
    d[i].set_string(sentinels[i].data(), 3 + i); d[i].set_has_lob_header();
    before[i] = d[i]; untouched[i] = owned(OB_SUCCESS, d[i]);
  }
  const auto sentinel_before = sentinels;
  const auto meta_before = f.meta, rows_before = f.rows;
  std::printf("CASE\t%d\tlen=%d\thex=%d\tdesc=%d\tcap=7\tnull=0\tvar=1\tfail=get3\n",
      cases, length, int(hex), f.desc_count);
  const int ret = f.decoder.batch_decode(ctx, &index, ids.data(), nullptr, cap, d.data());
  std::printf("B\t%d\tpartial\t%d\n", cases, ret);
  need(ret == OB_ERR_UNEXPECTED && index.gets == prefix + 1 && index.batches == 0,
      "forwarded row-index failure ordering/return");
  need(a.calls == 1 && a.blocks.size() == 1 && a.blocks[0].size == f.stride * cap,
      "failure must occur after successful allocation");
  auto check_scalar = [&](int i, const char *kind) {
    const char *row = nullptr; int64_t len = 0; ObDatum one;
    need(f.index()->get(ids[i], row, len) == OB_SUCCESS, "scalar row locate");
    ObBitStream bs(reinterpret_cast<unsigned char *>(const_cast<char *>(row)), len);
    const int sr = f.decoder.decode(sc, one, ids[i], bs, row, len);
    const auto s = owned(sr, one); emit(kind, i, ids[i], s);
    need(owned(OB_SUCCESS, d[i]) == s && s == f.oracle(ids[i]), "published/scalar/source mismatch");
  };
  for (int i = 0; i < cap; ++i) {
    auto b = owned(OB_SUCCESS, d[i]); b.ret = ret; emit("partial-batch", i, ids[i], b);
    if (i < prefix) {
      check_scalar(i, "scalar-prefix");
      need(d[i].ptr_ == reinterpret_cast<char *>(a.blocks[0].raw + 16 + i * f.stride), "prefix row slot");
    } else need(d[i].pack_ == before[i].pack_ && d[i].ptr_ == before[i].ptr_
        && owned(OB_SUCCESS, d[i]) == untouched[i], "failed/unvisited suffix changed");
  }
  const auto failed = d;
  const int retry = f.decoder.batch_decode(ctx, f.index(), ids.data(), nullptr, cap, d.data());
  std::printf("B\t%d\tretry\t%d\n", cases, retry);
  need(retry == OB_SUCCESS && a.calls == 2 && a.blocks.size() == 2
      && a.blocks[1].size == f.stride * cap, "same-array retry allocation/return");
  for (int i = 0; i < cap; ++i) {
    saved[i] = owned(retry, d[i]); emit("retry", i, ids[i], saved[i]); check_scalar(i, "scalar-retry");
    need(d[i].ptr_ == reinterpret_cast<char *>(a.blocks[1].raw + 16 + i * f.stride), "retry row slot");
  }
  need(sentinels == sentinel_before && f.meta == meta_before && f.rows == rows_before, "input/sentinel mutation");
  for (const auto &b : a.blocks) for (int i = 0; i < cap; ++i)
    for (int j = length; j < f.stride; ++j) need(b.raw[16 + i * f.stride + j] == 0xa5, "error/retry slack canary");
  std::fill(f.meta.begin(), f.meta.end(), 0xec); std::fill(f.rows.begin(), f.rows.end(), 0x9b);
  for (int i = 0; i < cap; ++i) need(owned(retry, d[i]) == saved[i], "retry aliases encoded input");
  for (int i = 0; i < prefix; ++i) need(owned(OB_SUCCESS, failed[i]) == f.oracle(ids[i]), "retry clobbered prefix");
  a.check(); scalar.check();
}
} // namespace
int main()
{
  try {
    ContextScope context;
    const int lengths[] = {1, 2, 16, 64, 127, 128, 129, 255}, caps[] = {0, 1, 2, 7, 64};
    for (int l = 0; l < 8; ++l) for (int h = 0; h < 2; ++h) for (int c = 0; c < 5; ++c)
      run(lengths[l], h, c % 4, (l + h + c) % 4, caps[c]);
    for (int h = 0; h < 2; ++h) for (int p = 0; p < 4; ++p) for (int n = 0; n < 4; ++n)
      run(64, h, p, n, 7);
    for (int len : {1, 128, 129, 255}) for (int h = 0; h < 2; ++h) for (int n = 0; n < 4; ++n)
      run(len, h, 2, n, 7, true);
    for (int len : {127, 128, 129}) for (int h = 0; h < 2; ++h) run(len, h, 1, 0, 2);
    for (int h = 0; h < 2; ++h) run(64, h, 2, 1, 7, false, true);
    need(cases == 152, "incomplete original corpus");
    for (int len : {16, 64}) for (int h = 0; h < 2; ++h)
      for (int n = 0; n < 4; ++n) for (int cap : {2, 64}) run(len, h, 2, n, cap, true, false, true);
    for (int h = 0; h < 2; ++h) for (int n = 0; n < 4; ++n) run(64, h, 2, n, 64, false, false, true);
    for (bool var : {false, true}) for (int h = 0; h < 2; ++h)
      for (int n = 0; n < 4; ++n) for (int cap : {2, 64}) run(65, h, 2, n, cap, var, false, true);
    for (int len : {16, 64, 65}) for (int h = 0; h < 2; ++h) run_partial_failure(len, h);
    need(cases == 230, "incomplete extended corpus"); std::printf("PASS\tcases=%d\n", cases);
    return std::fflush(stdout) == 0 && !std::ferror(stdout) ? 0 : 1;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "FAIL case=%d %s\n", cases, e.what()); return 1;
  }
}
