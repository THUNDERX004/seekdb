# Round 37：受限全文索引构建排序

保留的生产提交为 `2e4434ec7230d29aebb992bc0e7b1f2dfc521d0a`。
只对受支持的 FTS 辅助表布局开放相应 DDL 排序路径；同时保留 binary
单 NUL 编码修正和 Windows 标量比较回退。公开来源归属如下：
PR2358（`wwwyhyhyh/seekdb-2026`）的
`ca66bb259de29e2a377d6ece00a135d04ec7108e`、
`86cd2ab9604e39ca409159ed32b06461fc58b01c` 提供编码排序思路；
PR2302（`liwan2/seekdb`）的 `6aeef682f433e93baaee21986e06525d82442f7a`
提供 FTS 范围门结构及 binary 单 NUL 修正。本地做了适配，不将这些公开改动
归为本地原创；已有 OceanBase 版权和 Apache-2.0 文件头保持完整。

父任务证据：native 原实现以 `0xC0000409` 退出，标量候选通过 51,480 个案例。
官方 SQL 重现 **a6** 保存两次 EXPLAIN 和 encode-off 的完整 1,027 行后，
encode-on 断连，进程退出码同为 `0xC0000409`。a1–a5 均是执行 SQL 前的
父 harness 失败。修复候选的完整 off/on 结果和原版 off 参照比较通过，
服务保持存活；未取得调试器调用栈。

最终生产库链接的 native 凭证
`parent-build-currentlinked-1/native-run-currentlinked.json` 记录
`pass_bounded_candidate_core`：13,686,384 对，core/diagnostic 差异和错误均为 0，
1,586,970 次 guard 检查及 64 个 witness。使用当前生产 encoder archive 与真实
datum comparator；仅证明候选的有限集合，该凭证的 native baseline 为 NOT RUN，
不据此宣称 SQL 调用点覆盖或穷尽 collation 等价。

本地仅一个完整 A/B，affinity 15，使用未改动的 20,000 行固定参数：
三项构建耗时从 **7.996/6.156/3.570 秒降至 7.402/5.184/3.004 秒**。
构建类下降 13.0241766%，分词类 1.4965925%，查询类 0.4757707%，
三类等权综合下降 4.9988466%。这是相对已接受 R33 二进制的一组增量测量，
不产生正式成绩或稳定性结论；未修改指标的波动不归因为排序收益。
这里不是额外增加约 5 分，也不是相对官方原版的累计提升。

历史候选标签
`5bce9e06d1281347ad61344e4b01fab9158c58a2+round37-scoped-sort`
是错误的源码元数据，原凭证保留不改写。候选二进制 SHA-256
`5b511d7cbc882671c3508839da9162e1e7940056ac34a68ef4fb7bc269302095`
与成功 fixed-repro 的凭证一致，后者标识保留提交 `2e4434ec`。
父任务将其对应到已接受 R33（`f1f93b43`）叠加 R37，不能称为仅基于官方原版的候选。
A/B 对照二进制 SHA-256 为
`596b93cd688e9ab47449391db1dfeb5acfd56095cee558b064f8d5a4a03f7437`。
身份映射依据二进制哈希及冻结源码/补丁，而非错误标签；官方原始二进制仍是另一个
`bae4a3b9…` 身份。

父任务已通过原侧边检查：bin/general_ci 两表各 22 文档。
新增独立仓库测试保留相同输入、完整字段字节及 OR 命中集合检查，
并用不受 Python 优化影响的条件校验有限正分数。本次编写没有执行新增文件：

```sh
python unittest/storage/fts/test_encoded_fts_build.py --port 2891
```

需要 PyMySQL，拒绝 2881，仅重建并清理 `fts_encoded_build` 数据库。
检查真实 FTS 索引构建和每个返回行的全部存储字段，不是只比较命中数量。
精确 BM25 分数、collation 顺序、普通索引访问、落盘/错误路径和实际编码排序
执行仍不在此小测试的证明范围内；EXPLAIN 缺少标记不等于该路径未执行。
