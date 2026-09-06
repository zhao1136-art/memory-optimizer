# 高效内存优化工具（Memory Optimizer）开发与交付计划

## 一、项目概述（Summary）

开发一个**纯 C 原生**的内存优化工具，监控 **Windows 本机系统进程**，通过 **Web 管理后台** 提供直观界面，并开放 **REST API** 以便集成。工具本身低内存、低开销。

第一期范围按用户要求 **一次做全**：覆盖实时监控、泄漏检测、热点分析、对象生命周期追踪、碎片统计、自动化优化建议（智能回收 / 大对象策略 / 缓存管理）、报告导出，并交付源代码、用户手册、API 文档、性能测试报告。

## 二、现状分析（Current State Analysis）

- 工作目录 `g:\内存优化\ncyh` 为空，是全新项目（greenfield）。
- 已确认可用工具链：
  - 编译器：`F:\winlibs_mingw-16.1.0posix-14.0.0-ucrt-r3\mingw64\bin\gcc.exe`（gcc 16.1.0，x86_64-w64-mingw32）
  - Make：`...\bin\mingw32-make.exe` 4.4.1（注意：无 `make.exe`，仅 `mingw32-make.exe`）
  - 调试器 `gdb`、资源编译器 `windres`、`dlltool` 均在。
  - Python 3.14.4（`F:\Python314\python.exe`）可用于辅助生成/测试，但核心为 C。
- 约束：无 `git`；`winget` 受沙箱限制；外部网络下载不可靠 → **不依赖任何外部第三方链接库/CDN**；前端图表用原生 Canvas/SVG 手写，不引入在线图表库。

## 三、技术选型与架构（Architecture）

纯 C（C11，MinGW-w64）实现核心引擎与 HTTP 服务，前端为单页静态资源（HTML+CSS+原生 JS+Canvas 图表），由 C 引擎内置 HTTP 服务器直接伺服。

分层：
- **采集层**：`NtQuerySystemInformation`（运行时动态加载，避免依赖 ntdll.lib）一次系统调用获取所有进程的内存/句柄/线程/CPU 计数；PSAPI `GetProcessMemoryInfo`+`GetPerformanceInfo`+`GlobalMemoryStatusEx` 补充分项。
- **分析层**：采样线程维护时间序列环形缓冲（有界、低内存）；泄漏检测（趋势回归）、热点分析、碎片统计、运行环境识别、优化规则引擎。
- **服务层**：WinSock 多线程 HTTP/1.1 服务器，路由到 REST JSON 接口 + 静态资源。
- **展现层**：单页 Web 后台（`/` → index.html），原生 Canvas 图表，无弹窗、少页面。

低开销保证：采样本进程只有一个轻量采样线程；`NtQuerySystemInformation` 单调用扫描全部进程；时间序列为定长环形缓冲。

## 四、目录结构与模块设计（Proposed Changes）

```
g:\内存优化\ncyh\
├── include\            # 头文件
│   ├── memtool.h       # 公共定义：配置结构、进程快照结构、错误码
│   ├── sampler.h leak.h hotspot.h fragment.h optimize.h
│   ├── report.h storage.h
│   ├── http_server.h routes.h json.h runtime.h counters.h snapshot.h
├── src\
│   ├── main.c          # CLI 入口：启动采样引擎 + HTTP 服务
│   ├── config.c        # 加载 memtool.conf，默认值
│   ├── snapshot.c      # NtQuerySystemInformation 全进程快照
│   ├── counters.c      # PSAPI 内存/CPU 计数器、系统级统计
│   ├── sampler.c       # 采样线程 + 定长环形缓冲时间序列
│   ├── leak.c          # 泄漏检测：趋势回归 + 句柄增长 + 陡增标志
│   ├── hotspot.c       # 热点：top 占用、峰值、增长率
│   ├── fragment.c      # 堆碎片：PEB→ProcessHeap→UCR/free list（尽力而为，需提权）
│   ├── runtime.c       # 按命令行识别运行环境(runtime)类型
│   ├── optimize.c      # 优化建议规则引擎 + 安全动作执行
│   ├── report.c        # 生成 HTML/JSON/CSV 报告
│   ├── storage.c       # 滚动持久化 + CSV 导出（定长文件、按容量轮转）
│   ├── json.c          # 轻量 JSON 序列化/解析
│   ├── http_server.c   # WinSock 多线程 HTTP/1.1 服务器
│   ├── routes.c        # REST 路由分发 + 鉴权 + CORS
│   └── web\            # 静态前端（单页）
│       ├── index.html
│       ├── app.js      # 原生 JS + Canvas 图表
│       └── style.css
├── Makefile            # 供 mingw32-make
├── build.bat           # 一键构建（gcc -O2）
├── run.bat             # 启动（默认仅本地）
├── memtool.conf        # 示例配置
├── docs\
│   ├── 用户手册.md
│   ├── API文档.md
│   └── 性能测试报告.md
```

### 模块职责与关键算法

1. **snapshot/sampler（实时监控）**
   - `snapshot_system()` 用 `GetProcAddress` 动态取 `NtQuerySystemInformation(SystemProcessInformation)`，解析为进程数组：PID、父PID、名称、工作集(WorkingSet)、私有字节(PrivateBytes)、页文件、Page Faults、句柄数、线程数、CPU Kernel/User 时间、虚拟大小、PeakWorkingSet。
   - `counters_*`：PSAPI 获取指定进程详细内存 + 系统级 `GetPerformanceInfo`(物理/提交/页文件/缓存) 与 `GlobalMemoryStatusEx`。
   - 采样线程默认 `interval_ms=2000`，写环形缓冲（默认窗口 3600 点，进程首尾有界，内存开销恒定）。
2. **leak（泄漏检测）**
   - 对每个被监控进程在窗口内对 `private_bytes`、`working_set`、`handles` 做最小二乘线性回归；斜率超阈值且连续 N 个窗口保持、同时 CPU 稳定（排除负载波动）→ 判为泄漏候选。
   - 输出：泄漏评分/严重度、估计泄漏速率(KB/min)、起止时间、回归拟合度 R²。
   - 同时检测“句柄数持续增长”“私有字节持续增长但工作集不释放”两类异常。
   - 说明：外部进程“泄漏定位到具体分配点”需运行时插桩，进程级工具无法做到，故定位于**进程级定位 + 趋势取证**，此为明确假设。
3. **hotspot（热点分析）**：Top-K 进程按私有字节/工作集/峰值排序；增长率、峰谷值、相对系统占比；输出可排序可筛选列表。
4. **fragment（碎片统计）**
   - 对目标进程 `OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ)` → 读 PEB → ProcessHeap 偏移 → 遍历 `HEAP_LIST` → 汇总 UCR（Committed/Free List）总提交字节与空闲字节统计 → 碎片率 = 空闲块数/总提交块数（或空闲字节占比）。
   - x64 PEB/HEAP 结构偏移采用固定常量表；受保护进程或缺权限时返回“权限不足”，优雅降级，不崩溃。需以管理员运行以覆盖绝大多数进程。
5. **runtime（运行环境识别）**：按进程命令行/可执行名校验常见运行时（python/jvm/java/node/python 各发行版等），为“多种语言”提供进程级兼容识别，为后续语言级适配器预留 `adapter` 钩子（API 预留 `runtime` 字段）。
6. **optimize（优化建议）**
   - 规则引擎输出建议清单（带可解释原因与预期收益）：
     - **智能回收**：某进程私有字节低而工作集异常高（空闲驻留页多）→ 建议 `SetProcessWorkingSetSize(h,-1,-1)` 收割；系统可用内存低且 standby/缓存巨大 → 建议回收。
     - **大对象策略**：提交字节大 / 峰值爆发、最高区域占用大的进程 → 建议启用/增大大块连续分配、压缩驻留页等。
     - **缓存管理**：系统缓存(standby)占比高、可用内存低 → 建议释放缓存/队列化写缓存等。
   - 安全动作执行：`apply` 接口对指定 PID 执行 `SetProcessWorkingSetSize` 收割；需相应权限，失败返回明确错误，不影响工具运行。
7. **report（报告）**：生成含热点、泄漏、碎片、建议、系统总览的 HTML 报告（离线可读），并支持 JSON/CSV 导出时间序列。
8. **storage（持久化）**：窗口摘要按容量轮转落盘；CSV 导出；保留配置小时数后清理。
9. **http_server/routes（服务与 API）**：见下节。

## 五、REST API 设计（API文档将列全细节）

前缀 `/api/v1`，JSON。鉴权：默认仅 `127.0.0.1` 免密；`allow_remote=1` 时强制要求 `Authorization: Bearer <token>`（token 在 memtool.conf 配置）；CORS 白名单限制，不用 `*`。前置拦截：非白名单 Origin 拒绝。

| 方法与路径 | 说明 |
|---|---|
| GET `/api/v1/overview` | 系统总览 + Top 进程摘要 |
| GET `/api/v1/processes` | 全部进程快照（支持 `?sort=&top=`）|
| GET `/api/v1/process/{pid}` | 单进程详情 |
| GET `/api/v1/process/{pid}/trend` | 时间序列（泄漏图数据）|
| GET `/api/v1/leaks` | 泄漏候选列表 |
| GET `/api/v1/hotspots` | 热点分析 |
| GET `/api/v1/fragmentation/{pid}` | 堆碎片统计 |
| GET `/api/v1/runtime` | 运行环境/运行时识别结果 |
| GET `/api/v1/suggestions` | 优化建议清单 |
| POST `/api/v1/suggestions/apply` | 对某 PID 执行安全动作（收割工作集）|
| GET `/api/v1/report?format=html\|json\|csv` | 生成/下载报告 |
| GET `/api/v1/sampler` | 采样器状态（间隔、窗口、已采点数）|
| GET `/` 及静态资源 | Web 管理后台 |

## 六、前端单页设计

单页 SPA（无弹窗、无自动保存密码弹窗，页面保持在 1 屏内分区块）：
- 顶栏：系统总览（物理内存/可用/提交/页文件 + 轻量 Canvas 波动曲线）。
- 进程表：可排序/搜索，行内小热度条，点击行展开详情（趋势图、碎片、建议、runtime 标识）。
- 泄漏检测面板：候选进程列表 + 趋势回归曲线。
- 热点面板：Top-K 条形图。
- 碎片面板：选中进程的提交/空闲/碎片率。
- 建议面板：规则建议卡片 + “应用”按钮（仅安全动作）。
- 报告区：一键生成/下载 HTML 报告。
- 手动刷新 + 自动轮询（默认 2s，与采样间隔对齐）。

## 七、安全与健壮性（复用既有约定）

- 提权场景：碎片采集与跨进程动作要求管理员，未提权时优雅降级并给出中文提示，不崩溃。
- 网络服务默认绑定 `127.0.0.1`；`allow_remote=1` 必须配 token；CORS 白名单；不做 `*`。
- 路径校验：报告/导出路径规范化，拒绝绝对路径与 `..`，防目录穿越。
- 子进程调用避免弹窗（本工具无 PowerShell 依赖，必要时加 `CREATE_NO_WINDOW`）。
- 核心用动态加载 `ntdll!NtQuerySystemInformation`，避免 MinGW 无 ntdll.lib 问题。

## 八、假设与决策（Assumptions & Decisions）

1. **技术栈**：纯 C(C11)+MinGW-w64，无第三方链接库；前端手写 Canvas，无 CDN。
2. **“多语言支持”落地**：进程级（语言无关）+ 按命令行识别运行时类型，并预留语言适配器钩子；语言内部对象级内省（如 JVM 堆、Python 对象图）不在本交付范围。
3. **“定位泄漏”落地**：定位于进程级定位与趋势取证（增长率、R²、时长、句柄异常），不承诺到源码分配点。
4. **跨进程堆读取**：碎片统计为尽力而为，需管理员与 `PROCESS_VM_READ`；受保护进程返回“权限不足”。
5. **监控目标**：本机 Windows 进程；采样线程仅一个，开销低。
6. 配置项（采样间隔、窗口大小、鉴权、存储轮转）集中 `memtool.conf`，默认值即上述。

## 九、构建与运行（Build & Run）

- **构建**：`build.bat`（调用 gcc `-O2 -std=c11 -Isource -lws2_32 -lpsapi -o bin\memtool.exe`）与 `Makefile`（`mingw32-make`）。输出到 `bin\`。
- **运行**：`memtool.exe --serve --config memtool.conf`；默认监听 `127.0.0.1:8080`，浏览器打开 `http://127.0.0.1:8080`。
- 常用 CLI：`--snapshot`（单次全景）、`--report <格式>`（生成报告）、`--watch PID`（指定关注进程）、`--demo`（生成模拟负载进程用于演示/自测）。

## 十、验证步骤（Verification）

1. **编译自检**：`build.bat` 无警告/错误；exe 生成。
2. **功能自测**：
   - 用 `--demo` 生成一个有泄漏特征与一个高驻留的测试进程；
   - 验证 overview/processes/leaks/hotspots 数据与系统任务管理器接近；
   - 验证泄漏候选被检出且趋势图正确；
   - 验证建议清单与 `apply`（收割工作集）在测试进程上生效且可用内存回升。
3. **API 自测**：对上述端点逐一请求，校验 JSON 结构、鉴权（放行/401/403）、CORS 拒绝非白名单 Origin。
4. **健壮性**：对受保护 PID 请求碎片接口→返回权限不足不崩溃；非法 PID/路径→明确错误。
5. **性能测试**：在本机运行工具静置 M 分钟，测量工具自身 CPU 占用与内存占用（记录到性能测试报告）；用脚本连续打 API 测延迟与吞吐。
6. **交付物核查**：源代码 + 用户手册 + API 文档 + 性能测试报告齐全，文档与实现一致。

## 十一、分阶段实施

1. 骨架：`memtool.h` 公共结构/配置、`json.c`、`http_server.c`（健康检查）、`build.bat`、`Makefile`、静态首页占位 → 先跑通“能编译、能打开网页”。
2. 采集：`snapshot.c`+`counters.c`+`sampler.c` → `/overview`、`/processes`、`/process/{pid}`、`/process/{pid}/trend` 出真实数据，前端出曲线。
3. 分析：`leak.c`、`hotspot.c`、`runtime.c` → `/leaks`、`/hotspots`、`/runtime`。
4. 进阶：`fragment.c`（提权尽力而为）、`optimize.c`（规则 + apply）、`storage.c`、`report.c`。
5. 联调收尾：鉴权/CORS/路径校验完善；`--demo` 自测；性能测试并撰写三份交付文档。