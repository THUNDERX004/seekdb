# 实验记录

以下R001—R014为上一轮提交666989f7d的历史记录，非当前验证。该轮二进制 020d7c11b4a0e56ac5c2f3ca88bfcc16de7c6311e653ae49233084d27717142d；证据与边界见EXPERIMENT_RESULTS.md和validation-metadata.json。

| ID | 目的 | 版本/负载 | 状态 | 实际结果 |
| --- | --- | --- | --- | --- |
| R001 | 远端与基线 | 官方a3dba559c；本地3659b39d | DONE | 再核实官方分支、祖先和mysqltest无修改 |
| R002 | 基线与单机制消融 | baseline/correctness/stream/serialization/storage/filter各3轮 | DONE | 原始JSON保留；旧bench.py口径仅诊断 |
| R003 | CPU/内存/I/O诊断 | 独立profile构建和新evaluate.py | DONE_WITH_LIMITS | gprof无函数采样；Windows计数非cgroup |
| R004 | 隐藏语义边界 | 默认及scalar | DONE | 各17项边界、6070条差分及既有恢复测试通过 |
| R005 | 加长随机 | 3种子各4000次操作 | DONE | 26999条SQL，0 mismatch |
| R006 | 开源方法增量 | 查询probe预转换/每查询dispatch | DONE | 早期三轮配对查询倍率中位数1.0763，未调用外部索引库 |
| R007 | 混合量级精度 | 整数真值、16维反例与极端输入 | DONE | 两版各6项/497SQL/416排序SELECT；oracle2项自检 |
| R008 | 参考测试纠错 | 旧float64减法假并列 | DONE | 保留16次mismatch失败日志；独立Fraction自检后强化排序oracle重测通过 |
| R009 | 精度检查扫描开销 | 缓存向外舍入拒绝界 | DONE | 数学静态复核及全部回归通过，完整负载重新测量 |
| R010 | 最终主负载 | 10000行/1000查询/3轮 | DONE | 写入中位数比5.5861，查询1.8500；全部查询/持久化检查通过 |
| R011 | 最终超缓存 | 1MiB/3000行/40查询/3轮 | DONE | 查询中位数比18.6674；全部检查通过 |
| R012 | 驱动断言验证 | 列名/空{}/行值/计数 | DONE | 3项mock回归通过，官方mysqltest不修改 |
| R013 | 正式ARIS跨模型审计 | 所需reviewer MCP | REVIEW_UNAVAILABLE | 同模型审查不冒充正式审计 |
| R014 | Docker/Linux/官方隐藏/cgroup | 当前环境/完整驱动缺失 | NOT_RUN | 不计算正式E/名次，不消耗正式提交机会 |

## 第三轮（当前交付）

最终默认二进制88ecccd8adf6f53102ae446d3d65377427a18038a25bb28a6704d6db142dd424；本地对照666989f7d，官方基线9f6df2ba5acf871376e22b64a3d0724e4b575044。

| Run ID | 目的 | 证据 | 状态 | 实际结果/边界 |
| --- | --- | --- | --- | --- |
| R3-01 | 新公开合同 | mysqltest；ROUND3_PLAN_20261109_185500.md | DONE | 原样合并20组95 SQL；9组正式保留输入不可见 |
| R3-02 | 独立诊断 | profile_output/round3-{before,final}-{topk,ordinary}.json | DONE_WITH_LIMITS | 普通投影调用200000→200；非排他CPU采样 |
| R3-03 | 延迟投影/安全所有权 | round3-selection.json；round3-selection-ordinary.json | DONE | 最终仅入选行投影，缓存/overlay借用不越过同步SELECT |
| R3-04 | batch四行距离 | round3-batch.json | REJECTED | 配对查询倍率中位数0.9823，最终移除 |
| R3-05 | 固定块磁盘扫描 | round3-disk.json；round3-final-disk.json | DONE | 有ORDER时按offset重排；无ORDER不重排；检查CRC/当前版本 |
| R3-06 | Row指针目录 | round3-directory.json | REJECTED | 两轮慢一轮快，配对倍率中位数0.9360，最终移除 |
| R3-07 | 提交故障边界 | round3-validation.json；round3-validation-final.json | DONE_AFTER_RETEST | 初次FAILED记录保留；测试专用触发误命中setup，收窄多表条件后全量重测 |
| R3-08 | 默认和标量回归 | round3-validation-final.json/.txt | DONE | 共19命令通过；各20组95公开SQL、6070差分、精确排序、550查询选择路径 |
| R3-09 | 默认加长随机 | 同上normal/long-random | DONE | 27034 SQL；合常规33104，0 mismatch；标量未跑加长部分 |
| R3-10 | 提交故障构建/生产隔离 | 同上commit-failure-regressions | DONE | 6场景19子进程；半WAL完整回滚、发布中断完整恢复、noop不增长WAL |
| R3-11 | 最终主负载 | round3-final-main.json | DONE | 三轮全查询/重启全字段检查；写入中位数-2.76%，查询-4.18% |
| R3-12 | 最终超缓存 | round3-final-disk.json | DONE | 查询中位数比2.0738；写入中位数略慢；不是OS冷缓存 |
| R3-13 | 普通排序附加负载 | profile_output/round3-ordinary-{before,final}-{1,2,3}.json | DONE | 未插桩各5查询，三轮全字段校验；非官方主负载 |
| R3-14 | 机械数值/版本核验 | round3-summary.json；.aris/evidence_precheck.json | DONE | 核对文件和数值存在，不是质量裁决 |
| R3-15 | 正式跨模型裁决 | CLAIMS_FROM_RESULTS.md；findings.md | REVIEW_UNAVAILABLE | 指定reviewer MCP未注册，无正式裁决 |
| R3-16 | Linux/Docker/cgroup/隐藏输入 | EXPERIMENT_RESULTS.md | NOT_RUN | 不计算正式E、排名或保证全部29正式case |

未指定目录的结果JSON位于bench/results/，验证日志位于refine-logs/。
