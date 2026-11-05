# 第33轮计数全文查询隐式排序——保留（2026-11-05）

保留经过边界修正的 R33。父任务凭证记录生产源码、测试及官方 fixture 已提交推送至 `f1f93b43176bf62f3a883d082ee86eb9d68f867a`。对受限形状的 COUNT(*) 限行全文派生表，预处理可省略自动合成的 MATCH DESC 排序；显式排序及值消费场景保留既有路径。目标计划仍计算 relevance、保留 LIMIT，避免回表，并记录精确 LIMIT 参数约束。

修正后仅接受已知、有符号、非负整数 LIMIT/OFFSET，使用减法检查两者之和不超过 UINT32_MAX。不支持的表示及过大边界继续走原验证路径。跳过隐式排序前须完成既有参数精确值约束注册，注册错误向上传播；这修复了初版绕过旧验证、把错误变为成功计数的问题。

相对已接受 R32 的三组完整增量配对，目标 LIMIT 平均耗时 **2.481967 → 0.706700 ms**，下降 **71.526612%**；逐组为 +75.056730%、+67.954220%、+71.049435%。另有三组独立累计配对，以官方原版 `5bce9e06d1281347ad61344e4b01fab9158c58a2` 为对照。

| 观测耗时下降 | 增量相对 R32 | 累计相对官方原版 |
| --- | ---: | ---: |
| 构建类 | +7.037943% | +6.650805% |
| 分词类 | +18.067508% | +21.132691% |
| 查询类 | +28.526104% | +57.719835% |
| 三类等权综合 | +17.877185% | +28.501110% |

两类比较范围分开陈述，未改动指标的波动不能归因为本规则。全部带符号结果保留，包括增量第三组构建退化；不删除异常组、不修正计时、不扣除 SELECT1。两套各六次运行均为 ROWS20000/BATCH500/ROUNDS3000/QUERY_ROUNDS200/SAMPLES3/WARMUP30/SKIP_LOAD0，凭证核对 affinity15 及预期命中数。累计结果也仅属本地证据，**不是新的正式或满分成绩，不作统计稳定性声明**。

初版54项完整差异保留：索引10、11、30、31在极端 prepared LIMIT/OFFSET 上，官方返回1210而候选返回计数。bounded104项随后与官方完整记录一致。初次 bounded regression-a1 的相对 reference 路径 FileNotFoundError 仍标失败，a2成功另述。ranked432、keytypes192、R32的399例 focused 与七套既有回归均保留实际 action 凭证，不把含失败动作的组合运行写成全通过。

测试 v1 与 Major 审查原样收录：Python 优化会删除 assert 门，使差异或空 reference 可能以成功状态退出。这是源码证明，不声称实际演示过 v1 错误 PASS。V2 改为无条件验证/抛错，并为 literal 各方向阶段使用不同 alias。最终官方和候选正门与官方 fixture 的104项完整有序结果/错误码一致：40项 prepared、22项即时重用、28项 literal-reuse、14项变体。旧未修正候选因21项差异失败，空 reference 因不满足104项要求失败。版本、审查、fixture、完整输出及负门凭证均保留。

**`r33-bounded-candidate-final-test-v2-opt-a1` 不能称为优化模式运行**：父任务明确说明当时未设置 PYTHONOPTIMIZE。父任务指定 `...-opt-a2` 使用 PYTHONOPTIMIZE=1，提交汇总凭证也声明优化模式下正门通过、差异及空 reference 负门失败。单次 experiment receipt 没有环境或 sys.flags.optimize 字段，因此这部分模式归属来自父任务执行说明，不能由标签独立推断。计划包含地址，不声称完整计划或 stdout 哈希一致；未证明实际缓存命中、分配失败注入或所有输入等价。修正后的静态复查未发现新的具体 Major/Blocker。

[精确字节归档](../assets/fts-performance/competition-round33-count-only-order/manifest.json) 收录 R33 全部快照、补丁版本、helper、审查、计时与原始输出、构建日志、失败历史及 commit helper 源码；归档者不执行这些工作负载或提交脚本。生产快照 SHA-256 为 `408fac5f05ca084168b3d1f45df567c33cbadfe42317df5b008d019461b544ac`；提交的 v2 测试 LF SHA-256 为 `800cecebf41834341e2e3102b6041b8f11ac4e70fdef3d9a9c902e16eef40f8d`。历史 commit/dirty 运行标签不改写，实验身份以源码/补丁固定哈希为准，后续父凭证记录晋升。二进制哈希仅沿用凭证，不重新计算；排除 R34/R35/publicresearch、native 二进制、运行数据与秘密信息，不宣称当前 checkout 干净或后续轮次已验证。
