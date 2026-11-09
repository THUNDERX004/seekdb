# Research Findings

## 2026-11-09 第三轮执行事实与边界

- 正式审计与结果主张裁决：REVIEW_UNAVAILABLE。已检查注册工具，没有mcp__codex__codex/codex-reply或所需reviewer MCP；没有实际评审调用、threadId或裁决，不生成伪造PASS/WARN/FAIL报告。不能用同模型subagent当跨模型评审团，不私自安装/切换外部付费后端。
- result-to-claim在此停止；CLAIMS_FROM_RESULTS.md仅含机器可读的一行。工程实现、测试命令与数值存在性记录可以交付，不宣称正式审计通过或正式提交就绪。
- .aris/claims.json及evidence_precheck.json是独立机械存在性检查；verified只指数字/文件存在，不是主张获得支持。
- 初次数值检查8项verified、1项value_not_found，原始evidence_precheck_initial.json保留：验证stdout只写20组，不直接输出95总数，原引用文件不含95字面值。改为引用对原样官方manifest的独立机械求和记录public-contract.json（20组、95 SQL），没有修改测试/真值或数字。
- 规划与保留结果：延迟投影/安全Row借用/移动候选、按offset固定块扫描、提交副本削减及写后异常fail-stop。最终报告是对上一轮666989f7d的本机增量，不是官方性能基线。
- 负实验：四行距离batch配对倍率中位数0.9823；Row目录0.9360。均移除，原始日志保留。不要在没有新瓶颈证据时重复加回两者。
- 初次故障验证FAILED保留；测试注入误命中单表准备阶段，收窄到多表目标场景后重建并完整复测，没有放宽SQL真值。
- 默认33104条差分SQL为6070常规+27034加长，0 mismatch；scalar只跑常规6070。公开20组95 SQL通过，不保证9组保留输入。
- 内存峰值略增、超缓存写入中位数略慢；不以查询改善掩盖这些副作用。1MiB行缓存不是总内存硬限额，metadata/事务overlay/大结果排序仍可能增长。
- 本机无可用Docker daemon/WSL/Linux；未做容器构建、cgroup v2或正式平台预检，未消耗提测机会。下一步在固定Linux环境核对镜像/协议/真实资源、受限内存恢复和大排序。
- 工程完成由退出码、文件指纹、Git差异/祖先及push外部结果检查（Type-A）决定；跨模型质量裁决（Type-B）仍未获得。
