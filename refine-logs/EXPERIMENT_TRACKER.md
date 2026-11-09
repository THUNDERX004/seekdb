# 实验记录

最终二进制 020d7c11b4a0e56ac5c2f3ca88bfcc16de7c6311e653ae49233084d27717142d；证据与边界见EXPERIMENT_RESULTS.md和validation-metadata.json。

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
