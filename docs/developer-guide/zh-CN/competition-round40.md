# 第 40 轮：顺序遍历 IK 仲裁链

基于官方 `upstream/2026-competition`（`5bce9e06d1281347ad61344e4b01fab9158c58a2`），增量对照版本为 `0fe1940d63fb7d4b38a9e267ef01719798c881c1`。

IK 仲裁原先将链放入哈希表，并在输出时逐字符查询。输入 token 已按偏移排序，仲裁形成的冲突组互不相交，选出的链仍位于对应组内，因此可以按顺序保存链，通过单调递增的下标查找当前偏移。改动使用 arena 分配的指针数组，初始预留有界，按实际链数扩容，减少哈希表创建和查找。字符解码、分类、补单字、仲裁选择和 token 输出顺序保持原样；保留重复初始化的错误返回。

思路参考 `juanzhi-gcb/seekdb` PR1787 的固定提交 `97ab48bb741f789e4e9d1d88380727f11cd03aaa`，本轮仅修改 `ob_ik_arbitrator.h/.cpp` 的链容器与访问方式。

## 必要验证

Windows x64 增量编译通过（本地 `OB_ENABLE_STANDBY=OFF`）。专用实例通过既有官方原版结果 fixture 的完整比较：

- `test_dictionary_prefixes.py`：810 组完整 IK 输出，包括四会话并发。
- `test_hot_path.py`：408 组 token、144 组排序查询、920 组谓词查询结果、144 组并发解析；token、频次、文档长度及 MATCH 完整结果一致。

## 单次本地性能对照

为减少重复测试，复用刚完成的 R38 控制组 `r39-config-control-a1`，候选只运行一遍完整官方基准。两侧均为独立新数据目录、CPU 4、内存 2G、日志盘 2G、WARN、亲和性 15；参数为 20,000 文档、BATCH 500、ROUNDS 3000、QUERY_ROUNDS 200、SAMPLES 3、WARMUP 30、SKIP_LOAD 0。官方测试文件保持不变。

| 指标 | 控制组 | 候选 |
| --- | ---: | ---: |
| build_ik_all_sec | 6.991 | 6.462 |
| build_ik_content_sec | 5.213 | 5.231 |
| build_beng_en_sec | 2.991 | 2.993 |
| tokenize_ik_avg_ms | 0.1320 | 0.1086 |
| tokenize_beng_avg_ms | 0.1272 | 0.1128 |
| query_cn_avg_ms | 0.9937 | 1.0158 |
| query_beng_avg_ms | 1.5353 | 1.3075 |
| query_mixed_avg_ms | 0.9874 | 1.1152 |
| query_limit_avg_ms | 0.8435 | 0.8574 |

IK 标题与正文构建耗时下降 7.57%，仅正文构建基本持平。三类等权综合观察值为下降 5.47%，其中未改动链路存在波动，不能全部归因于本轮优化，也不能与其他轮增量相加。查询命中数为 8001/11000/7332/20，仅作辅助核对，正确性以以上完整结果回归为依据。

候选二进制 SHA256：`42922f6970f139a0d9bdf2173905ef548067d08495fc16ea24e754f7a7783ac3`。本地结果不代表正式 Linux 平台成绩，尚未验证 50/50 满分。
