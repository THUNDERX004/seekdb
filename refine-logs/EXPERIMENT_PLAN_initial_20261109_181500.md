# MiniVectorDB 性能优化实验计划

日期：2026-11-09。基线：3659b39d6de0537fb81bd08a70b6f580c4e26f37。官方远端仍为a3dba559c1e7ebdc48bc990ffb1a4c75d1c1075c。

目标：降低写入和TopK查询时间、内存及I/O，保留SQL、事务、持久化、恢复、磁盘回退与精确检索语义。

## 实验与门槛

| 实验 | 设置 | 门槛 |
| --- | --- | --- |
| 功能基线 | 全部公开与现有扩展测试 | 全部通过 |
| 热点分析 | 独立profiling构建、CPU/内存/I/O诊断 | 可核对热点 |
| 性能对照 | 10000行、128维、1000查询、K=10、每批100行 | 同种子、三轮串行、两组Recall>=0.95 |
| 边界回归 | 大数距离、多排序键、更新删除、事务恢复 | 独立真值一致 |
| 候选复测 | 全部测试与三轮对照 | 无回归、主要指标改善 |

保存每轮原始结果，报告中位数与范围，不挑最好成绩。每次改变一个机制。

## 验证边界

正式E=(E_write+2*E_query)/3，各项为0.6*f(R_T)+0.2*f(R_B)+0.2*f(R_M)，f(R)=R/(1+R)。缺少官方Linux cgroup v2统计与固定基线，Windows计数仅作诊断，不预测名次。隐藏用例和完整驱动未公开，不能声称正式准入通过。当前无Docker/WSL运行环境。

## ARIS

应用experiment-plan、system-profile、analyze-results。experiment-audit与result-to-claim所需跨模型reviewer MCP不可用，记录REVIEW_UNAVAILABLE；子代理复核不冒充正式跨模型审计。
