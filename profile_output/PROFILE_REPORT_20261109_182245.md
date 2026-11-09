# 性能诊断报告

日期：2026-11-09T18:22:45+08:00。最终二进制 020d7c11b4a0e56ac5c2f3ca88bfcc16de7c6311e653ae49233084d27717142d。

## 诊断事实

| 维度 | 基线中位数 | 候选中位数 | 边界 |
| --- | ---: | ---: | --- |
| 写入进程CPU秒 | 1.6875 | 0.21875 | Windows子进程计数，非函数采样 |
| 查询进程CPU秒 | 0.328125 | 0.171875 | 量化计数，无调用栈 |
| 主负载peak working set字节 | 15486976 | 15503360 | 进程生命周期峰值，不是cgroup内存 |
| 主负载peak commit charge字节 | 11563008 | 11403264 | 不是磁盘pagefile读写量 |
| 1MiB缓存查询墙钟秒 | 6.8366545 | 0.3662347 | 超行缓存，不保证OS冷缓存 |

GetProcessIoCounters包含stdin/stdout管道，不能从中推算物理磁盘B。本机没有Linux perf/cgroup测量；GPU/互连不适用于该单进程CPU任务。

## 归因与局限

写入CPU明显下降，与保留的CRC/解析/序列化/流同步消融方向一致；这些历史消融使用bench.py且部分版本合并改动，不能精确归因单个函数。低缓存扫描大幅改善，与复用文件流消除逐行open/close一致。有过滤查询的改善与减少候选扫描一致。均为机制分析，不是函数热点采样证明。

独立-pg profiling构建运行后，gprof平面报告只有表头、没有函数采样。原输出见gprof-sampling.txt；无有效函数热点图，不能声称gprof确认某函数占比。对应profile构建、gmon.out留在被忽略的build/profile目录，未提交二进制。正式负载没有启用-pg或循环内计时。

查询预转换/每查询dispatch的三轮增量中位数配对加速1.0763倍；最终近等距离精确兜底后，对明显更远的行缓存拒绝界，消除每行重复精度判断。最终三轮与全部正确性证据见refine-logs/EXPERIMENT_RESULTS.md；历史较慢版本均保留。

下一步按优先级：官方Linux容器预检与cgroup v2资源采集；受限内存恢复与metadata/事务overlay压力测试；只有采样确认CPU扫描仍主导时才评估batch距离或新索引。不得通过绕过fsync、遗漏过滤、丢失精确排序或延迟计量外写回换性能。

## 诊断/插桩改动清单

| 文件 | 类型 | 内容 |
| --- | --- | --- |
| bench/evaluate.py | 新增 | 同种子串行交替、逐请求计时、Windows CPU/内存/I/O快照、独立全查询与持久化检查、二进制指纹 |
| bench/results/*.json | 新增原始数据 | 每轮实际计时、参数、摘要与诊断，旧结果未删除 |
| profile_output/gprof-sampling.txt | 新增 | 无有效采样的原始gprof平面输出 |
| profile_output/PROFILE_REPORT*.md | 新增 | 本报告及时间戳版本 |
| build/profile/minivectordb.exe、gmon.out | 本地忽略产物 | 独立-pg构建，不是提测二进制，不提交 |

生产源码没有PROFILE计时、stdout诊断或采样探针；内核优化不是插桩。诊断脚本可在不影响数据库运行的情况下不执行，故本轮没有删除用户产物。
