# MemOptimizer · REST API 文档

> 版本：v1.0 ｜ 基础路径：`http://<host>:<port>/api/v1` ｜ 默认 `http://127.0.0.1:8080/api/v1`
> 所有接口返回 JSON（报告导出接口除外）；字符编码 UTF-8。

---

## 一、总览

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/v1/overview` | 系统总览 + Top 进程摘要 + 采样器状态 |
| GET | `/api/v1/processes` | 全部进程快照（支持排序/搜索/截断） |
| GET | `/api/v1/process/{pid}` | 单进程详情 |
| GET | `/api/v1/process/{pid}/trend` | 单进程时间序列（趋势/泄漏图数据） |
| GET | `/api/v1/leaks` | 泄漏候选列表 |
| GET | `/api/v1/hotspots` | 内存热点 Top-K |
| GET | `/api/v1/fragmentation/{pid}` | 指定进程堆碎片统计 |
| GET | `/api/v1/runtime` | 运行环境/运行时识别汇总 |
| GET | `/api/v1/suggestions` | 优化建议清单 |
| POST | `/api/v1/suggestions/apply` | 对指定进程执行安全动作 |
| POST | `/api/v1/optimize` | 一键优化（回收工作集+关闭僵尸进程+清理缓存） |
| GET | `/api/v1/report` | 生成/下载报告（html/json/csv） |
| GET | `/api/v1/sampler` | 采样器状态 |
| GET | `/` 及静态资源 | Web 管理后台 |

---

## 二、鉴权与安全

### 2.1 访问控制

| 场景 | 规则 |
|---|---|
| 本机访问（127.0.0.1 / ::1 等） | 默认免密，直接放行 |
| `allow_remote=0` 时的远程访问 | 返回 `403 仅允许本机访问…` |
| `allow_remote=1` 时的远程访问 | 必须携带 `Authorization: Bearer <api_token>`，否则返回 `401 无效的访问令牌` |
| CORS | 配置了 `cors_origin` 后，非白名单 Origin 返回 `403 未在白名单的 Origin`；放行时响应头携带 `Access-Control-Allow-Origin: <origin>` |

### 2.2 错误响应格式

非 2xx 一律返回 JSON：

```json
{"error": "仅允许本机访问。如需远程请设置 allow_remote=1 与 api_token。"}
```

### 2.3 错误码

| 状态码 | 含义 |
|---|---|
| 200 | 成功 |
| 400 | 参数错误（如缺少 pid、非法路径） |
| 401 | 远程访问令牌无效 |
| 403 | 本机限制 / Origin 不在白名单 / 非法路径 |
| 404 | 接口不存在 / 进程不存在 / 静态文件不存在 |
| 405 | 方法不允许（预留） |
| 413 | 请求体过大 |
| 500 | 服务端错误 / 执行动作失败 |

---

## 三、字段约定

### 3.1 进程对象（process）

| 字段 | 类型 | 说明 |
|---|---|---|
| `pid` | int | 进程 ID |
| `ppid` | int | 父进程 ID |
| `name` | string | 可执行文件名 |
| `runtime` | string | 运行时类型标签（见 3.3） |
| `private_mb` | number | 私有字节（MB） |
| `ws_mb` | number | 工作集（MB） |
| `peak_mb` | number | 峰值工作集（MB） |
| `pagefile_mb` | number | 页文件占用（MB） |
| `faults` | int | 累计缺页数 |
| `handles` | int | 句柄数 |
| `threads` | int | 线程数 |
| `cpu_pct` | number | 最近采样窗估算 CPU 占用（%） |

### 3.2 系统对象（system）

| 字段 | 类型 | 说明 |
|---|---|---|
| `total` | object | `{ text: "3.95 GB", mb: 4040.758 }` 物理内存总量 |
| `avail` | object | `{ text: "558 MB", mb: 558.441 }` 可用物理内存 |
| `avail_pct` | number | 可用率（%） |
| `commit_mb` | number | 已用提交（MB） |
| `commit_limit_mb` | number | 提交限额（MB） |
| `cache_mb` | number | 缓存/standby（MB） |
| `handles` | int | 系统句柄总数 |
| `threads` | int | 系统线程总数 |

### 3.3 运行时类型（runtime）

`Unknown` / `Python` / `JVM/Java` / `Node.js` / `PowerShell` / `CMD` / `.NET` / `Go` / `Rust` / `Native`

### 3.4 严重度（severity）

泄漏与建议的严重度 0~5：`4~5` 高，`2~3` 中，`0~1` 低。

---

## 四、端点详解

### 4.1 GET /api/v1/overview

系统总览 + Top 10（按私有字节）+ 采样器状态。

**响应示例（实测）**

```json
{
  "system": {
    "total": {"text": "3.95 GB", "mb": 4040.758},
    "avail": {"text": "558 MB", "mb": 558.441},
    "avail_pct": 13.820,
    "commit_mb": 3941.480,
    "commit_limit_mb": 7368.758,
    "cache_mb": 356.219,
    "handles": 56023,
    "threads": 1501
  },
  "top": [
    {"pid": 912, "name": "lsass.exe", "runtime": "Native", "private_mb": 7.059,
     "ws_mb": 19.781, "peak_mb": 19.793, "cpu_pct": 0.0, "share": 0.002}
  ],
  "sampler": {"interval_ms": 2000, "window_size": 600, "sample_count": 3, "uptime_s": 6}
}
```

| 字段 | 说明 |
|---|---|
| `top[].share` | 该进程私有字节占全部进程私有字节比例（0~1） |
| `sampler.sample_count` | 已采样次数 |
| `sampler.uptime_s` | 服务运行秒数 |

---

### 4.2 GET /api/v1/processes

全部进程快照。

**查询参数**

| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `sort` | string | - | 预留排序字段（当前由前端排序，服务端返回原始顺序） |
| `search` | string | 空 | 按进程名子串过滤 |
| `top` | int | 2000 | 返回条数上限（1~2000） |

**响应示例（实测）**

```json
{
  "count": 145,
  "processes": [
    {"pid": 0, "ppid": 0, "name": "[System Process]", "runtime": "Native",
     "private_mb": 0.0, "ws_mb": 0.0, "peak_mb": 0.0, "pagefile_mb": 0.0,
     "faults": 0, "handles": 0, "threads": 4, "cpu_pct": 0.0}
  ]
}
```

---

### 4.3 GET /api/v1/process/{pid}

单进程详情。

- `{pid}` 为进程 ID。
- 进程不存在：`404 {"error": "进程不存在"}`
- 权限不足：`200` 且 `process.permission_limited = true`

**响应示例（实测，System 进程）**

```json
{"process": {"pid": 4, "ppid": 0, "name": "System", "runtime": "Native",
 "private_mb": 0.184, "ws_mb": 0.148, "peak_mb": 2.496, "pagefile_mb": 0.184,
 "faults": 3639, "handles": 0, "threads": 179, "cpu_pct": 1035.673}}
```

---

### 4.4 GET /api/v1/process/{pid}/trend

单进程时间序列（环形缓冲，最多返回末尾 2000 点）。

- `{pid}=0` 时返回系统级序列。
- 无数据：`200 {"pid":..., "error": "无该进程数据"}`（无 `points` 字段）。

**响应示例（实测）**

```json
{"pid": 4, "points": 4,
 "t": [13432466123.135, 13432466125.162, 13432466127.192, 13432466129.218],
 "priv_mb": [0.184, 0.184, 0.184, 0.184],
 "ws_mb": [0.148, 0.148, 0.148, 0.148],
 "cpu": [0.0, 789.344, 788.177, 1530.109]}
```

| 字段 | 说明 |
|---|---|
| `t[]` | 时间戳（epoch 秒，浮点） |
| `priv_mb[]` | 私有字节（MB） |
| `ws_mb[]` | 工作集（MB） |
| `cpu[]` | CPU 占用（%），仅 `pid != 0` 时返回 |

---

### 4.5 GET /api/v1/leaks

泄漏候选列表（按严重度降序，最多 50 条）。

**响应示例（实测，无泄漏）**

```json
{"count": 0, "leaks": []}
```

**有候选时的条目字段**

| 字段 | 类型 | 说明 |
|---|---|---|
| `pid` | int | 进程 ID |
| `name` | string | 进程名 |
| `kind` | string | `私有字节` 或 `句柄` |
| `rate_kb_min` | number | 增长速率（KB/min，或句柄数/分换算） |
| `r2` | number | 线性拟合优度（0~1） |
| `span_sec` | number | 观测历时（秒） |
| `cpu_pct` | number | 估算 CPU（%） |
| `severity` | int | 严重度 0~5 |

---

### 4.6 GET /api/v1/hotspots

内存热点 Top-K。

**查询参数**

| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `metric` | string | `private` | 排序指标：`private` / `working` / `peak` |
| `top` | int | 10 | 返回条数（1~100） |

**响应示例（实测）**

```json
{"metric": "private", "count": 3, "hotspots": [
  {"pid": 3676, "name": "avp.exe", "runtime": "Native", "private_mb": 418.066,
   "ws_mb": 225.117, "peak_mb": 508.059, "cpu_pct": 14384.350,
   "threads": 133, "share": 0.139}
]}
```

> 说明：`cpu_pct` 由最近采样窗的 CPU 时间差除以时间差得到；采样点不足或时间窗极短时可能出现异常大值，属瞬时估算特征。

---

### 4.7 GET /api/v1/fragmentation/{pid}

指定进程堆碎片统计。

**响应示例（实测，受保护进程）**

```json
{"pid": 4, "ok": false,
 "error": "权限不足（需要 PROCESS_QUERY_INFORMATION 访问权限，受保护/其他用户进程请以管理员运行）"}
```

**成功时字段**

| 字段 | 类型 | 说明 |
|---|---|---|
| `ok` | bool | 是否成功 |
| `region_count` | int | 已提交区数量 |
| `committed_mb` | number | 已提交字节（MB） |
| `largest_region_mb` | number | 最大连续已提交区（MB） |
| `reserved_mb` | number | 保留字节（MB） |
| `frag_idx` | number | 外部碎片指数（0=整块，越接近 1 越碎片化） |

---

### 4.8 GET /api/v1/runtime

各运行时类型进程数汇总。

**响应示例（实测）**

```json
{"runtimes": [
  {"runtime": "Unknown", "count": 0},
  {"runtime": "Python", "count": 0},
  {"runtime": "JVM/Java", "count": 0},
  {"runtime": "Node.js", "count": 0},
  {"runtime": "PowerShell", "count": 1},
  {"runtime": "CMD", "count": 0},
  {"runtime": ".NET", "count": 0},
  {"runtime": "Go", "count": 0},
  {"runtime": "Rust", "count": 0},
  {"runtime": "Native", "count": 144}
]}
```

---

### 4.9 GET /api/v1/suggestions

优化建议清单（按严重度降序，最多 30 条）。

**响应示例（实测）**

```json
{"count": 1, "suggestions": [
  {"severity": 3, "kind": "缓存", "pid": 0, "name": "system",
   "title": "系统可用内存偏低",
   "reason": "系统可用物理内存仅 13.9%（561 MB），缓存/standby 约 8.8%（355 MB）。建议释放/压缩系统缓存、关闭后台高驻留应用，或增加物理内存。",
   "actionable": false, "action": ""}
]}
```

| 字段 | 说明 |
|---|---|
| `severity` | 严重度 0~5 |
| `kind` | `回收` / `大对象` / `缓存` / `泄漏` / `系统` |
| `pid` | 目标进程（`0` 表示系统级） |
| `actionable` | 是否可安全执行（有对应动作） |
| `action` | 动作标识（当前仅 `reclaim`） |

---

### 4.10 POST /api/v1/suggestions/apply

对指定进程执行安全动作。

**请求体（JSON）**

```json
{"pid": 1234, "action": "reclaim"}
```

| 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `pid` | int | 是 | 目标进程 ID |
| `action` | string | 否 | 动作标识，当前支持 `reclaim`（默认） |

**响应**

成功：
```json
{"ok": true, "pid": 1234, "action": "reclaim", "message": "工作集回收已执行"}
```

失败（500）：
```json
{"ok": false, "pid": 1234, "action": "reclaim",
 "error": "权限不足，需要 PROCESS_SET_QUOTA（同用户进程或管理员）"}
```

---

### 4.11 POST /api/v1/optimize

一键优化：关闭僵尸进程 + 回收高驻留进程工作集 + 清理系统 standby 缓存（全部后端 C 实现）。

**请求体（JSON，可省略）**

| 参数 | 类型 | 必填 | 说明 |
|---|---|---|---|
| `options` | int | 否 | 位标志：`1`=回收工作集，`2`=关闭僵尸进程，`4`=清理缓存；省略或 `0` 表示全部执行 |

**响应示例（实测）**

```json
{"ok": true, "reclaimed": 5, "killed": 0, "skipped_protected": 0, "skipped_perm": 1,
 "reclaimed_mb": 1949.355, "cache_purged": true, "killed_list": [],
 "detail": "[回收工作集] app.exe (pid=123, 释放约 194MB)\n[缓存] 系统 standby 缓存已清理\n"}
```

| 字段 | 说明 |
|---|---|
| `reclaimed` | 工作集回收成功的进程数 |
| `reclaimed_mb` | 回收动作涉及的驻留内存合计（MB） |
| `killed` | 关闭的僵尸进程数 |
| `killed_list` | 被关闭进程数组（`pid`/`name`，最多 16 条） |
| `skipped_protected` | 因保护名单/系统进程跳过数 |
| `skipped_perm` | 因权限不足跳过数 |
| `cache_purged` | 是否成功清理系统 standby 缓存 |
| `cache_error` | 清缓存失败原因（失败时存在） |
| `detail` | 可读执行明细 |

**说明**：僵尸进程判定为"长时间低 CPU（<1%）且高驻留（工作集≥64MB）"；系统关键进程（System/smss/csrss/winlogon/services/lsass/svchost/explorer 等）与 PID≤50 一律保护，不会误杀。清理缓存需管理员权限（SeProfileSingleProcessPrivilege），失败不影响其他动作。

### 4.12 GET /api/v1/report

生成/下载报告。

**查询参数**

| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `format` | string | `html` | `html`（离线报告）/ `json`（=overview）/ `csv`（时间序列） |
| `pid` | int | 0 | CSV 模式下指定进程（0=系统序列） |

- `format=html`：返回 `text/html`，完整离线报告（系统总览 + 热点 + 泄漏 + 建议）。
- `format=json`：等价于 `GET /overview`。
- `format=csv&pid=0`：系统序列，表头 `ts_ms,avail_bytes,commit_used,cache_bytes,handles`。
- `format=csv&pid={pid}`：进程序列，表头 `ts_ms,private_bytes,working_set,handles,cpu_ms`。

**响应示例（实测，csv 系统序列）**

```csv
ts_ms,avail_bytes,commit_used,cache_bytes,handles
13432466123135,590012416,4135731200,373518336,56034
13432466125162,587677696,4136747008,373436416,56023
```

---

### 4.13 GET /api/v1/sampler

采样器状态。

**响应示例（实测）**

```json
{"interval_ms": 2000, "window_size": 600, "sample_count": 4, "uptime_s": 6, "nseries": 145}
```

| 字段 | 说明 |
|---|---|
| `nseries` | 当前维护的时间序列数（含系统序列） |

---

### 4.14 静态资源

- `GET /` → Web 管理后台首页（index.html）。
- `GET /app.js`、`GET /style.css` 等 → 对应静态资源。
- 路径做了安全校验：拒绝 `..`、绝对 URL（`:`）、query/fragment 等，非法路径返回 400，文件不存在返回 404。

---

## 五、调用示例

```bash
# 系统总览
curl http://127.0.0.1:8080/api/v1/overview

# 进程列表（按名称过滤）
curl "http://127.0.0.1:8080/api/v1/processes?search=python&top=20"

# 单进程趋势
curl http://127.0.0.1:8080/api/v1/process/1234/trend

# 泄漏与建议
curl http://127.0.0.1:8080/api/v1/leaks
curl http://127.0.0.1:8080/api/v1/suggestions

# 一键优化（回收+关僵尸+清缓存）
curl -X POST http://127.0.0.1:8080/api/v1/optimize \
     -H "Content-Type: application/json" \
     -d '{}'

# 仅回收工作集
curl -X POST http://127.0.0.1:8080/api/v1/optimize \
     -H "Content-Type: application/json" \
     -d '{"options":1}'

# 执行工作集回收
curl -X POST http://127.0.0.1:8080/api/v1/suggestions/apply \
     -H "Content-Type: application/json" \
     -d '{"pid": 1234, "action": "reclaim"}'

# 下载 HTML 报告
curl -o report.html "http://127.0.0.1:8080/api/v1/report?format=html"
```

远程访问（需配置 `allow_remote=1` 与 token）：

```bash
curl -H "Authorization: Bearer <api_token>" http://<host>:8080/api/v1/overview
```

---

## 六、说明

- 所有数值单位：内存为 **MB**（bytes/1048576），时间为毫秒/秒（见字段说明）。
- `cpu_pct`、`share` 等为采样估算值，随采样窗口变化，不保证与任务管理器完全一致。
- 各列表接口均有数量上限（如 leaks 50、hotspots 100、suggestions 30、trend 2000 点），超出部分截断。
