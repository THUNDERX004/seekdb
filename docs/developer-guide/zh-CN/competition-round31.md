# 第 31 轮 advance 比较：拒绝（2026-11-05）

**拒绝，不晋升生产修改或既有测试。** 第一组完整原始对照的三类等权耗时下降为
**−2.796834%**。后续重建控制与候选比较 **−2.110622%** 仅属探索性比较，不能
替代原始按时间顺序执行的 pair。LIMIT 候选 **2.0064 ms**，重建控制 **1.9979 ms**，
没有收益。诊断版本的计时单独保留，不混入性能结论。全部样本和负值原样保留，
不修正计时；无关 BENG 波动的因果尚未证实。

真实链接 native 测试通过 **1,844 项 helper 用例**：625 次快速路径、1,219 次回退，
以及 1,844 次独立真实比较器 oracle 调用。完整 stdout 共 9,221 行，SHA-256 为
`ec8fbe41bfc2eca0e975754e634340abfac25ec0d499597c8be53cf5d5140b22`，stderr 为空。
**helper 等价不证明两个 iterator 调用点的实际覆盖、refill 或 advance 控制流。**
候选排名、键类型 SQL 和七组既有回归通过；执行计划、诊断 benchmark、console
benchmark 和 console ranked 记录均保留。未观察到 marker，但 instrumentation
未被独立验证，因此不能证明零执行。首次诊断编译因 `LOG_WARN` 需要 `ret` 而失败；
失败源码/日志、显式返回码日志修复和 stderr 修复均精确保留，不声称覆盖问题已解决。

构建一致性审计未证实具体编译/链接配置漂移；已记录参数和相邻 DLL 相同不证明
全部历史构建及加载运行库一致。归档保留变化的库内容哈希及历史凭证缺口。闭环记录
确认精确恢复已接受源检查点 `f56ddd03e66826919d8d9192bc4c0afece8843dd`，tracked
source diff 为空。已接受源重建在删除无用实验头文件和未晋升 TU **之前**通过，
不声称删除后又执行构建。接受的生产检查点仍为 f56。C7 第 25 轮累计
**26.155337%** 仅为历史本地结果，不是第 31 轮新成绩或正式分数。

[第 31 轮精确字节归档](../assets/fts-performance/competition-round31-advance/manifest.json)
保留八组实验标签、提案/评审、native helper 完整输出、诊断失败及修复、一致性审计
和恢复记录。历史待运行状态原样保留，由后续凭证说明实际执行；不作满分、统计
稳定性、全部输入或 OOM 等价声明。
历史 runtime commit/dirty 标签保持原样，实验源身份以冻结源码快照、补丁和哈希为准。
[第 25 轮历史归档](../assets/fts-performance/competition-round25-cumulative/manifest.json)。
