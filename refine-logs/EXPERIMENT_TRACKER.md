# 实验记录

以下R001—R014为上一轮提交666989f7d的历史记录，非当前验证。该轮二进制 020d7c11b4a0e56ac5c2f3ca88bfcc16de7c6311e653ae49233084d27717142d；证据与边界见EXPERIMENT_RESULTS_20261109_182245.md和validation-metadata.json。

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

## 第三轮（历史交付）

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

## 第四轮（当前工程候选，正式目标未完成）

对照3c1adfbf3；候选默认SHA256 f1fd4e9c348a3c0544ec13708b061dc654d59fe1d20f65d8c8d7149d1cee8d7c。完整数据见EXPERIMENT_RESULTS_20261109_213756.md。

| Run ID | 目的 | 证据 | 状态 | 实际结果/边界 |
| --- | --- | --- | --- | --- |
| R4-01 | 合同、官方版本 | ROUND4_PLAN_20261109_212744.md；git核查 | DONE_WITH_LIMITS | 官方9f6df2ba；mysqltest零差异；没有正式失败日志/SHA |
| R4-02 | 无换行分号/注释尾反例 | round4-stdio-before.txt | REPRODUCED | 9方法有17处失败断言；真实管道保持stdin打开 |
| R4-03 | 裸标识符反例 | round4-sql-before.txt | REPRODUCED | 10方法6过4失败，包含错误删除/写入及事务状态 |
| R4-04 | 最小修复及专项 | round4-stdio-after.txt；round4-sql-after.txt | DONE | 9+10项全部通过；默认协议、fsync不变 |
| R4-05 | 完整本地回归 | round4-validation.json/.txt | DONE | 默认/标量/故障共23命令全过，公开20组95SQL；默认差分33104、标量6070，均0mismatch |
| R4-06 | 主负载三轮 | bench/results/round4-main.json | DONE | 写入中位数−8.47%，查询−1.29%；全查询及重启全字段通过 |
| R4-07 | 超缓存三轮 | bench/results/round4-disk.json | DONE_WITH_REGRESSION | 查询中位数+2.61%，保留负项 |
| R4-08 | 超缓存固定七轮补测 | bench/results/round4-disk-confirm.json | DONE_WITH_LIMITS | 查询+0.25%；全部十轮合计+1.01%，不据此证明没有性能代价 |
| R4-09 | 子代理检查 | 工程只读复审及反例测试 | DONE_WITH_LIMITS | SQL/协议反馈已整合；存储子代理策略中止未重试，不作正式审计 |
| R4-10 | 正式结果/协议/Linux/cgroup | EXPERIMENT_RESULTS.md未验证项 | PENDING_EXTERNAL | 需要正式必要日志、锁定SHA、完整协议或平台预检；未擅用提测次数 |
| R4-11 | ARIS跨模型裁决 | 指定reviewer MCP不可用 | REVIEW_UNAVAILABLE | 不以同模型子代理替代裁决 |
