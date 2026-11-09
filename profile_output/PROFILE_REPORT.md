# 第三轮性能诊断报告

记录：2026-11-09 19:27:39 +08:00。正式计时使用未插桩默认二进制88ecccd8adf6f53102ae446d3d65377427a18038a25bb28a6704d6db142dd424；诊断构建不冒充交付二进制。上一轮报告PROFILE_REPORT_20261109_182245.md原样保留。

## 诊断事实与观察者效应

MVD_PROFILE仅独立编译启用：Execute/Parse/Select逐调用计时，Distance/Output每256次取一次样，Heap每32次取一次样；完整calls仍计数。嵌套墙钟不可相加，也不是排他CPU调用栈采样。确定性采样可能有偏，时钟和计数会改变执行成本，不外推热点百分比。Parse/Execute包含建表/插入，Select只含查询。

TopK：10000行、128维、1000查询、K10、seed20261109。两版全部查询独立校验，两个Recall组均1.0。原始round3-before-topk.json/round3-final-topk.json：

| 路径 | 对照calls | 最终calls | 对照采样数 | 最终采样数 |
| --- | ---: | ---: | ---: | ---: |
| execute | 1101 | 1101 | 1101 | 1101 |
| parse | 1101 | 1101 | 1101 | 1101 |
| select | 1000 | 1000 | 1000 | 1000 |
| distance | 5500000 | 5500000 | 21485 | 21485 |
| output | 10000 | 10000 | 40 | 40 |
| heap | 67171 | 67171 | 2100 | 2100 |

两版扫描距离次数相同，说明本轮主收益并非减少向量距离数量。保留行引用/移动堆减少候选行副本是机制解释，不把嵌套计时当作因果分解。

普通排序附加负载：10000行、128维、20查询，SELECT id,payload,emb，ORDER BY cat,payload LIMIT 10 OFFSET 7。两版逐查询全字段独立校验：

| 路径 | 对照calls | 最终calls | 对照采样数 | 最终采样数 |
| --- | ---: | ---: | ---: | ---: |
| execute | 121 | 121 | 121 | 121 |
| parse | 121 | 121 | 121 | 121 |
| select | 20 | 20 | 20 | 20 |
| distance | 0 | 0 | 0 | 0 |
| output | 200000 | 200 | 782 | 1 |
| heap | 0 | 0 | 0 | 0 |

诊断查询墙钟21.6901721→0.0255960秒；输出格式化调用200000→200。提前为淘汰候选格式化向量/字符串的浪费已消除。此值只属独立插桩负载。未插桩附加对照另使用每轮5查询，共3轮，原始round3-ordinary-{before,final}-{1,2,3}.json；不能称官方主负载提升704倍。

## 未插桩结果和资源边界

- 主负载三轮中位数：写入0.3031224→0.2947664秒，查询0.2040616→0.1955412秒。
- 1MiB行缓存查询0.3603338→0.1737551秒；固定256项Version块按offset扫描及相邻记录少seek与该改善一致，不意味着物理设备IO减少同样倍数。
- working-set峰值中位数15372288→15446016字节，commit-charge11452416→11620352字节，略增；超缓存写入中位数略慢。
- Windows进程计数不是cgroup v2 memory.peak/io.stat；GetProcessIoCounters含管道。无Linux perf、可用Docker daemon或WSL；没有CPU排他热点图。GPU/互连不适用于本单进程CPU程序。
- batch四行距离和连续Row指针目录未显示稳定收益，已移除。原始负实验保留，不以单轮/中位数之比挑选正结果。

完整原始三轮、均值/样本标准差、正确性和版本指纹见refine-logs/EXPERIMENT_RESULTS.md与bench/results/round3-summary.json。

## 诊断/插桩改动清单

| 文件 | 类型 | 内容 | 行 |
| --- | --- | --- | --- |
| src/minivectordb.cpp | 修改，仅宏启用 | ProfileCounter/ProfileScope、stderr结构化报告及关闭宏 | 36、57—111 |
| src/minivectordb.cpp | 修改，仅宏启用 | Execute/Parse/Select/Output/Distance/Heap采样作用域 | 1971、2429、2513、2628、2633、2643、3287 |
| bench/profile_queries.py | 新增 | 独立负载、请求计时、全字段真值、插桩原始stderr、指纹 | 全文件 |
| profile_output/round3-*.json | 新增 | 原始诊断/未插桩普通负载，区分binary hash | 各文件 |
| profile_output/PROFILE_REPORT*.md | 新增/更新 | 本报告及历史副本 | 全文件 |
| build/round3-profile-{before,final}/ | 本地忽略产物 | 独立宏构建；不提交二进制 | — |

MVD_TEST_FAULTS是另一个独立测试宏，不是性能诊断；仅故障构建启用。生产默认/标量构建不启用两宏，stdout协议不变。插桩已编译隔离，可在后续不需要诊断时按上表移除，不删除本轮原始证据。

下一步优先官方Linux容器预检、cgroup真实资源和受限内存恢复压力；只有新证据明确瓶颈后才尝试新索引/布局。不减fsync、不损失精确排序和过滤语义。正式跨模型审计REVIEW_UNAVAILABLE。
