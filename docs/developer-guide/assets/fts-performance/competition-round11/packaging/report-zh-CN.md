## 第十一轮检查点（2026-11-04）

本轮仅保留为**正确性已验证的小幅检查点**。`round11-three-pair-comparison.json` 的三组父版本/候选版相邻完整对照，三类等权综合耗时仅下降 **0.25310035648%**：构建下降 **1.2031%**、查询下降 **2.8238%**，但**分词退化 3.2676%**。尚未证明稳定增量收益或稳定满分，**没有新的正式平台成绩**；此前报告的性能原始分 21.099/100、加权约 10.550/50 仍是历史结果。本轮以第十轮仍物化结果的父版本为控制，属于本地增量对照，并非新的官方原版累计对照，不能与第十轮独立样本的百分比直接相加。

| 指标 | 物化父版本均值 | 最终候选版均值 | 耗时下降 |
| --- | ---: | ---: | ---: |
| `build_ik_all_sec` | 6.402000 | 6.391333 | 0.1666% |
| `build_ik_content_sec` | 5.079333 | 4.905667 | 3.4191% |
| `build_beng_en_sec` | 2.825333 | 2.824667 | 0.0236% |
| `tokenize_ik_avg_ms` | 0.136833 | 0.143400 | **-4.7990%** |
| `tokenize_beng_avg_ms` | 0.105600 | 0.107433 | **-1.7361%** |
| `query_cn_avg_ms` | 1.042000 | 0.998400 | 4.1843% |
| `query_beng_avg_ms` | 1.325100 | 1.286367 | 2.9230% |
| `query_mixed_avg_ms` | 1.105333 | 1.072200 | 2.9976% |
| `query_limit_avg_ms` | 2.192933 | 2.166833 | 1.1902% |

首组和前两组对照综合均为负结果，`round11-first-pair-comparison.json` 与 `round11-incremental-comparison.json` 保留这些阶段全部逐指标负提升：

| 保留的对照阶段 | 构建耗时下降 | 分词耗时下降 | 查询耗时下降 | 三类等权综合下降 |
| --- | ---: | ---: | ---: | ---: |
| 首组 | 0.3544% | -6.3665% | -1.6228% | **-2.54495%** |
| 前两组 | 1.1130% | -6.3251% | 1.9148% | **-1.09909%** |
| 全三组 | 1.2031% | **-3.2676%** | 2.8238% | 0.25310035648% |

六次完整运行全部保留。先按各指标运行均值计算耗时下降比例，再在每类内、三类间等权平均。未扣除 SELECT1、未截断负数、未挑选最快样本，也未用不完整运行替换样本。固定参数仍为 rows=20000、batch=500、rounds=3000、query_rounds=200、samples=3、warmup=30、skip_load=0，六份报告命中数均为 8001/11000/7332/20。性能实验使用 WARN 日志级别；诊断运行独立于计分样本。

改动只对符合条件、没有外部值消费者的顶层自然语言 IR 根节点省去外部 ID/相关度 datum 物化。代码生成在排除值依赖、残余过滤、lookup/回表、排序及其他元数据依赖后，才清空现有外部输出数组；内部 ID 表达式和检索元数据保留。运行时继续保守处理相关度、评分过滤、top-k、函数 lookup 和其他算法路径。运行时字段 `materialize_results_` 默认 true，没有新增序列化标志；DaaT 保持推进和计数，仅省去符合条件的 datum 写入与投影。

初次 LOG_DEBUG 构建虽然完成 SQL 诊断，但可执行文件中没有编译进诊断 literal，**不能用作运行路径证据**。其 `round11-source.patch`、元数据、构建日志和 diagnostic-1 实验及输出全部保留。最终 LOG_TRACE 构建的 **diagnostic-2** 与 `round11-runtime-materialization.log` 按诊断 SQL 顺序提供五条路径观测：三条无输出 COUNT 为 `materialize_results:false`，ID/排序和 LIMIT/top-k 保守控制为 `materialize_results:true`。该证据只证明观测到的这些路径，不涵盖所有优化器或分布式执行路径。诊断 mixed COUNT 因 SQL 不同为 6666，正式基准的 mixed 命中仍为 7332。

独立官方原版 fixture 是 `unittest/storage/fts/query_materialization_expected.json.gz`，由 `round11-original-query-record-3` 在官方提交 `5bce9e06d1281347ad61344e4b01fab9158c58a2` 上录制，原版可执行文件 SHA256 为 `bae4a3b9e1b4662d4021b28d5adc085764beb6cf02122eacd57b8e16b6fe1762`。最终候选通过**全部 4,416 组完整结果回归**，比较行、ID、表达式、计数、警告和原有 SQL 错误。驱动请求四种执行模式，不据此声称每种请求的优化器模式均实际生效。前两次原版录制失败也保留：先是 Python bool 列元数据序列化，随后是 bytes 转 Decimal，再在清理时出现包序号错误。第三次录制成功，未用候选输出制作期望。fixture SHA256：`8bb2ccc372e011a1fc290221d601857ca5093d4cdd5fc1449eae20b73b8a2c93`。

candidate-1 同时通过七套既有 SQL suite、BMP 的 2,488 组完整输出、Unicode/大小写的 16,359 组（含保留的 924 组原版错误）、排名的 432 组/142,692 行，以及主键类型的 192 组/68,852 行。以上均是已有运行日志，证据打包不重跑测试。

最终生产身份为父提交 `ecbf498d120dabaa01b60b89c49aa18a6b9926a1` 加 **`round11-trace-source.patch`**，补丁 SHA256 **`40ea9aa36f6efd0559d4efad439ece943822e32c19ef2dbd90a6337fb904deaf`**。最终可执行文件为 `build/fts-round11-trace-value-free/seekdb.exe`，SHA256 **`9c239e363020a77c201c31456c940084f948d5b5741ec73472af55632c9b91da`**，与候选实验元数据及独立读取的文件哈希一致。报告中的 `git_head` 仅指运行时检出目录，不替代已保存二进制的来源。父任务已告知：当前 `git diff --binary -- src` 与最终补丁逐字节一致，远端 `upstream/2026-competition` 仍为 `5bce9e06d1281347ad61344e4b01fab9158c58a2`，祖先检查通过且受保护文件无变化；包中明确归因父任务核验，打包侧不重跑这些检查。

[第十一轮证据](../assets/fts-performance/competition-round11/manifest.json) 保留所有 round11 原始文件、两份精确源码补丁、两份构建日志（含运行库 DLL 提示）、完整 proposal 原始快照及其当时仅离线验证的 manifest、原版 fixture、最终驱动、失败录制和诊断。proposal 及初始 `pending` 元数据保留历史原样，均不冒充最终生产身份。所有原始文件按字节复制，保留原始换行；压缩版生产补丁只是补充。manifest 逐文件记录 SHA256，`SHA256SUMS` 另外覆盖 manifest。未纳入第十二轮或后续证据。Windows 编译/链接日志不能证明 Linux CI、稳定加速、满分或新的平台成绩。
