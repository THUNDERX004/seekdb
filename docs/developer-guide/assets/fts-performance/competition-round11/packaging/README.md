# Round11 evidence packaging handoff

交付内容：`package-round11.py`、中英报告新增段落、测试 README 建议，及运行脚本后生成的逐字节证据包和两个待父任务应用的补丁。此目录内不含 round12 或后续文件。

```powershell
& 'C:/Users/11946/Documents/ChatGPT/OceanBase比赛/.venv/Scripts/python.exe' -X utf8 'C:/Users/11946/Documents/ChatGPT/OceanBase比赛/setup/fts/round11-package/package-round11.py'
```

默认输出同目录 `evidence/`，目录存在时拒绝覆盖。已有包可直接验证，不必重打：

```powershell
& 'C:/Users/11946/Documents/ChatGPT/OceanBase比赛/.venv/Scripts/python.exe' -X utf8 'C:/Users/11946/Documents/ChatGPT/OceanBase比赛/setup/fts/round11-package/package-round11.py' --verify 'C:/Users/11946/Documents/ChatGPT/OceanBase比赛/setup/fts/round11-package/evidence'
```

需要重打时用 `--output` 指定新的 C 盘输出目录。`--source-root` 默认 D:/Projects/OceanBase/seekdb，只读取 fixture、最终回归驱动、已保存二进制和报告/测试 README 原文；脚本禁止直接向该源码 checkout 输出。全程仅 Python 标准库，不启动服务、不执行构建/SQL/性能/Git，不应用任何补丁。

包内 `reports.patch` 只向中英 `fts-performance.md` 顶部新增 round11 章节，保留旧报告；`test-readme.patch` 单独追加测试 README 建议。`report-bases.json` 固定生成补丁时各文件原字节及 LF 哈希，便于父任务核对。供父任务在 checkpoint 前自行审阅、应用，随后按已有 Git 工作流提交和推送；打包脚本不会代做这些动作。

父任务可将经核验的 evidence 目录完整复制到 `docs/developer-guide/assets/fts-performance/competition-round11/` 后再纳入检查点。请勿只复制选中的成功记录。报告中的相对链接指向该最终资产位置。

打包规则：收集 setup/fts 顶层所有 round11-* **文件**，递归保留 round11-proposal 全部文件；原始字节不解码改写。初次 LOG_DEBUG/最终 LOG_TRACE 的精确 patch、构建/诊断、两次录制失败、第三次原版成功、全部六次对照均保留。另收集三个未执行 helper 快照、最终测试驱动和独立原版 fixture。proposal 当时的旧 HEAD、pending、未执行声明保持原样，并明确不是最终生产身份。

生产补丁与二进制、fixture、最终驱动均固定 SHA；二进制存在时重新读取文件核验，不存储体积较大的 exe。二进制文件不可用时保留实验元数据来源并以 observed_sha256=null 明示未重新读取；原字节、每个源文件 SHA 和大小记于 inventory.json。最终 patch 保留原文件，另提供 mtime=0 的 gzip。

脚本从已有报告重新计算首组、前两组和三组均值与完整负指标，并验证固定参数和命中、成功动作与两次录制失败、4416 原版 fixture 和 3 false/2 true 的现有 runtime 记录；这只是离线证据一致性检查，不是重新运行数据库测试。raw_file_count 和 proposal_file_count 可确认收集范围。生成途中若失败，保留部分目录用于排查，不自动删除。

manifest.json 的 files 覆盖所有 payload（含脚本、报告文本、父任务核验归因、两份待应用补丁和 inventory）；SHA256SUMS 还覆盖 manifest 自身。SHA256SUMS 不自哈希，manifest 不自哈希。`--verify` 同时检查逐文件 SHA、清单、缺失/额外文件。两次失败以非零 exit_code 记录，不能因为失败而排除；LOG_DEBUG 初试 exit_code=0 也明确不能证明路径。

最终口径：仅小幅检查点，综合下降 0.25310035648%、查询下降 2.8238%、构建下降 1.2031%、分词退化 3.2676%；首组及前两组为负，负指标全部保留；没有新正式成绩，也没有稳定满分结论。父任务补充的源 diff/远端/祖先/保护文件核验记录在 parent-attestation.json，明确是父任务已验证事项。
