<div align="center">

# MemOptimizer · 内存优化工具

**纯 C 写的 Windows 内存分析工具 —— 不是「一键释放几百 MB」的假优化，而是真的看进程内存去了哪。**

[![Language](https://img.shields.io/badge/language-C99-283593?logo=c&logoColor=white)](#)
[![Build](https://img.shields.io/badge/build-MinGW--w64%20gcc-6D4C41)](#)
[![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-0078D6?logo=windows&logoColor=white)](#)
[![Deps](https://img.shields.io/badge/dependencies-仅%20Win32%20API-2ea44f)](#)
[![API](https://img.shields.io/badge/HTTP%20API-127.0.0.1%3A8080-009688)](#)

</div>

---

## 为什么不是又一个「一键加速」

大多数「内存优化」软件做的事是调用 `EmptyWorkingSet` 把页面刷到磁盘，
任务管理器里数字好看了，**下次访问又全换回来，还更慢**。

这个工具走的是另一条路：**先测量，再决定动什么**。

| 模块 | 干什么 |
|---|---|
| `sampler.c` | 按进程持续采样工作集 / 提交大小 / 私有字节，形成时间序列 |
| `leak.c` | 从时间序列里识别**单调增长**的疑似泄漏（不是单点快照） |
| `fragment.c` | 分析虚拟地址空间的碎片化程度 |
| `hotspot.c` | 找出真正吃内存的热点进程，而不是按名字猜 |
| `optimize.c` | 针对性优化（按进程/按策略），不是无差别刷页面 |
| `optimize_oneclick.c` | 一键模式 —— 但仍然走上面那套判据，不是盲刷 |
| `protect.c` | 保护关键进程不被误优化（系统进程、当前前台程序等） |
| `snapshot.c` | 内存快照，前后对比看真实变化 |
| `report.c` + `pdf.c` | 生成报告（含 PDF 导出），把数据带走 |

## 内置 HTTP 服务

`memtool.exe` 启动后会在 **`127.0.0.1:8080`** 起一个本地 HTTP 服务，
把采样数据和操作接口暴露出去 —— 这样可以自己写前端、接监控、或做定时巡检。

```ini
# memtool.conf
[server]
host = 127.0.0.1
port = 8080
# 远程访问设为 1 时，必须配置 api_token（否则远程一律拒绝）
api_token =
allow_remote = 0
# CORS 白名单，逗号分隔；留空表示仅本机请求合法
cors_origin =
```

**安全默认**：默认只监听本机、`allow_remote = 0`、CORS 只允许本机。
放开远程时必须配 token —— 这一条是写死在代码里的，不配 token 就拒绝。

> 完整接口见 [`doc/API文档.md`](doc/API文档.md)

## 快速开始

```bat
:: 编译（需要 MinGW-w64 gcc）
build.bat

:: 运行
bin\memtool.exe
```

也可以直接跑 `run.bat`。已编译版本在 `release/memopt.zip`。

命令行参数：`--serve` 起服务、`-demo` 自测子进程、`-snapshot` / `--report` 做快照与出报告。

```bat
memtool.exe --serve            :: 起 HTTP 服务
memtool.exe -snapshot          :: 抓一次快照
memtool.exe --report out.pdf   :: 导出报告
```

## 项目结构

```
src/                    19 个 .c 模块
├── main.c                入口：CLI 解析 + serve 主循环
├── sampler.c             内存采样
├── leak.c                泄漏识别（时间序列）
├── fragment.c            碎片分析
├── hotspot.c             热点进程定位
├── optimize.c            优化策略
├── optimize_oneclick.c   一键优化
├── protect.c             关键进程保护
├── snapshot.c            快照
├── report.c / pdf.c      报告与 PDF 导出
├── http_server.c         HTTP 服务
├── routes.c              路由（28 KB，最大的一个）
├── json.c                极简 JSON 序列化
├── config.c              配置解析（memtool.conf）
├── storage.c / runtime.c / counters.c / util.c
inc/                    16 个对应头文件
doc/                    用户手册 / API 文档 / 软件介绍 / 性能测试报告 / 导出须知
data/                   运行时数据
bin/                   编译产物 memtool.exe
release/                打包发布版
```

**零第三方依赖** —— 只用 Win32 API 和 C 标准库。没有 libcurl、没有 JSON 库、
没有 Web 框架，HTTP 服务和 JSON 序列化都是自己实现的。

## 已知限制

- **仅 Windows** —— 全程 Win32 API（`Toolhelp32` / `NtQuerySystemInformation` 等），没有跨平台抽象。
- **需要管理员权限**才能采样/优化其他用户的进程；非管理员下只能看自己权限范围内的。
- **不做「释放内存」这种数字游戏** —— 刷 `EmptyWorkingSet` 让任务管理器数字变好看，实际会让后续访问变慢。这个工具不做。
- **`allow_remote=1` 且未配 `api_token` 时，远程请求会被拒绝**（有意为之，不是 bug）。

## 许可

**禁止商业使用。**

本项目采用 [PolyForm Noncommercial License 1.0.0](LICENSE)。

- **允许**：个人学习、研究、实验、业余项目；教学、公益、公共研究、政府部门等非商业组织使用。
- **不允许**：任何商业用途 —— 包括但不限于集成进商业产品、做成 SaaS 或收费服务、
  为企业内部商业运营提供支持。


> 本项目为个人项目，按「现状」提供，不附带任何担保。

---

<div align="center">
<sub>个人项目 · <a href="LICENSE">PolyForm Noncommercial License 1.0.0</a> · 禁止商业使用</sub>
</div>
