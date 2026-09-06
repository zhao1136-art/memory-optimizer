# 报告多格式导出功能实现计划

## 概述（Summary）

当前系统仅支持一个"生成报告"按钮，点击后用 `window.open("/api/v1/report?format=html")` 新开标签页查看。后端 [api\_report](g:\内存优化\ncyh\src\routes.c#L557-L570) 只支持 `html / csv / json`（json 实际是返回 overview 快照，不是完整报告），且全程无：

* 下载文件名（Content-Disposition）

* 格式统一的校验与错误处理

* 导出过程中的用户反馈（进度 / 成功 / 失败）

本方案把导出升级为**统一格式选择入口**，支持 **PDF / Excel(.xls) / CSV / JSON / HTML** 五类格式，确保数据完整、格式正确、样式一致，提供下载文件名、错误处理和前端反馈。

**用户已确认的取舍**（纯 C / 无第三方库前提下）：

* Excel → 生成 **HTML 表格 .xls**（Excel 可直接打开，含中文与样式）

* PDF → **原生 PDF 且含中文**（纯 C 手写，内嵌系统 simhei.ttf 的**字形子集**以控制体积）

* 导出入口 → **统一导出入口**（头部改为格式下拉 + 导出按钮，非每面板各自导出）

## 现状分析（Current State Analysis）

### 相关文件

| 文件                                      | 现状                                                           |
| --------------------------------------- | ------------------------------------------------------------ |
| `src/routes.c` `api_report()` L557-L570 | 仅 html/csv/json；json 实为 overview 快照；无 filename、无验证           |
| `src/report.c` / `include/report.h`     | 实现 `report_build_html()`、`report_build_csv()`；无 JSON/PDF/xls |
| `include/memtool.h`                     | `strbuf`、`sampler`、`sys_mem`、`proc_snap` 类型，JSON 序列化器        |
| `src/json.c`/`include/json.h`           | 栈式 JSON 序列化 + 精确取 key（用于 POST body）；`j_str` 已含 `\\u` 转义      |
| `src/web/index.html` L16                | 单个 `btnReport` 按钮                                            |
| `src/web/app.js` L222                   | `$("#btnReport").onclick = () => window.open(...)`           |
| `include/http_server.h`                 | `http_resp` 已有 `extra_headers`（动态字符串需保证存活）                   |
| `src/main.c` L153-L164                  | `--report <fmt>`：html/csv 分支可用，json 分支为空 stub                |

### 可复用的数据/函数

后端报告元素全部已有分析函数（在 sampler 锁内调用，与 report\_build\_html 一致）：

* 系统总览：`sys_mem sy = s->sys`

* 热点：`hotspot_rank(s, "private", 15, hh, 15)` → `hotspot_item{pid,name,rt,private_mb,ws_mb,peak_mb,share,cpu_pct}`

* 泄漏：`leak_detect(s, li, 10)` → `leak_item{name,kind,slope_kb_per_min,r2,time_span_sec,severity}`

* 建议：`optimize_analyze(s, sug, 20)` → `suggest_item{severity,title,reason}`

* 进程列表：`s->procs[]` / `s->nprocs`

* 采样信息：`s->interval_ms`、`s->window_size`、`s->sample_count`、`s->started_ms`

* HTML/CSV 已有，可直接复用为两种格式

## 拟改动（Proposed Changes）

### 1. 新增 JSON 完整报告 `report_build_json()`

在 `src/report.c` 新增 `char *report_build_json(sampler *s)`（malloc，调用方 free）。返回结构：

```json
{
  "generated_ms": <wall_ms>,
  "sampler": {"interval_ms":.., "window_size":.., "sample_count":.., "uptime_s":..},
  "system": {"total_mb":.., "avail_mb":.., "avail_pct":.., "commit_used_mb":.., "commit_limit_mb":.., "cache_mb":.., "handles":.., "threads":..},
  "hotspots": [ {"pid":..,"name":"..","runtime":"..","private_mb":..,"ws_mb":..,"peak_mb":..,"cpu_pct":..,"share":..} ],
  "leaks":     [ {"name":"..","kind":"..","slope_kb_per_min":..,"r2":..,"time_span_sec":..,"severity":"高|中|低"} ],
  "suggestions":[ {"severity":"..","title":"..","reason":"..","actionable":bool,"action":".."} ],
  "processes": [ {"pid":..,"name":"..","private_mb":..,"ws_mb":..,"peak_mb":..,"cpu_pct":..} ]
}
```

* 复用 `json` 序列化器（`j_begin_obj/j_key/j_str/j_dbl/j_bool/j_end`）。

* 文件操作与 `report_build_html` 相同：`sampler_lock()` 包围读取，`strbuf` 输出，`dup_s` 收尾。

* `kind` 映射：`li[i].kind == 0 ? "private" : "handles"`；`severity_str()` 已有，可复用到 PDF/json。

### 2. 新增 xls 导出 `report_build_xls()`

在 `src/report.c` 新增 `char *report_build_xls(sampler *s)`。

* 内容为 HTML 表格（`.xls` 本质是 HTML，Excel 兼容打开带样式）。

* 头部：`<meta charset="utf-8">` + `<table border=1>`，含系统总览 / 热点 / 泄漏 / 建议四个区块（与 `report_build_html` 数据一致，保证"界面与导出一致"）。

* MIME 用 `application/vnd.ms-excel`，文件名 `memopt_report.xls`。

* 表格样式：内联 `border-collapse:collapse; cellpadding` + 表头 `<th>` 背景色，保证"跨格式一致样式"。

* 中文内容经 `html_escape_tmp()` 转义复用。

### 3. 新增原生 PDF（含中文）`report_build_pdf()`

新增 `src/pdf.c` + `include/pdf.h`（新文件），核心：

* **函数**：`char *report_build_pdf(sampler *s)` 返回 PDF 字节（malloc）。

* **中文字体子集化（重点）**：

  * 启动/首次导出时从 `C:\Windows\Fonts\simhei.ttf` 读取。

  * 解析 TrueType 的 `cmap` / `head` / `maxp` / `hhea` / `loca` / `glyf` 表，**只提取报告实际出现的字符**对应的字形，构建一个精简子集字体（Type0 / CIDFontType2，`Identity-H` 编码，`FontFile2` 内嵌子集 `head`/`hhea`/`maxp`/`cmap`→`CIDToGIDMap`/`glyf`/`loca` 子集）。

  * 报告出现的中文字符集合：把系统/热点/泄漏/建议中的**进程名 + 中文标签**清洗为一个 `unicode 集合`（用 `struct set{ uint32_t *arr; int n; }` 去重）。

  * 未找到字体时**降级**：生成纯 ASCII PDF（表头与英文标签 + 拉丁字符），并标记 `pdf_font_embedded=false`，其余数据不变——保证"转换失败有降级而非失败空白"。

* **布局**：简单流式 `BT/ET` + `Tj`，按行排布标题、系统总览、热点表、泄漏表、建议。坐标以 `pt` 计算（A4 宽 595、顶部起始），自动折行。

* **转义**：PDF 文本流需转义 `\\ ( )`；`Identity-H` 下字符按 2 字节 UTF-16BE 写出，子集表只含出现的字符（无 BOM 的 UTF-16BE）。

* 体积控制：只内嵌出现的字形，典型报告预计几百 KB 而非 10MB 原字体。

字体子集化若工程过大，**备选**：退化为嵌入**精简后 ASCII/Glyph 子集**（同上做法仅提取**需要的字符**），保证中文仍显示。若连子集化都不可行则启用 ASCII 降级。计划默认走"真子集化"。

（注：在计划读代码阶段，进行探索以定位 TTF 表解析均基于标准 TrueType 格式，不依赖第三方库。）

### 4. 后端 `api_report()` 扩展

`src/routes.c`：

* 读取 `format`：`html|csv|json|pdf|xls`（缺省 html）。

* 每种格式生成对应字符串并 `text_resp`，`content_type` 按格式映射；**pdf/xls/csv/json 一律附加** **`extra_headers`**：`Content-Disposition: attachment; filename="memopt_report.<ext>"`。

* **动态 header 生命周期**：`extra_headers` 是指针，不能指向栈。做法：把下载头拼进一个 `static _Thread_local` buffer（或由生成函数返回的结构里持有），在本连接处理结束、发送完成后仍有效——`http_resp.body` 已用 `free_body=1`，header 用 `_Thread_local` 静态缓冲，安全且线程安全（每连接一线程）。

* 未知格式 → `json_err(400, "不支持的导出格式")`。

* `memtool_config` 增加 `pdf_font` 字段（默认 `C:\Windows\Fonts\simhei.ttf`），可在 conf 覆盖。

**测试用直连钩子**：`api_report` 内对 `format=json` 改为调用 `report_build_json`（不再是 overview）。

### 5. CLI `--report` 扩展

`src/main.c` L153-L164：

* `--report json` 调用 `report_build_json` 输出到 stdout。

* `--report pdf`、`--report xls` 同样支持（写 stdout）。

* 与 serve 共用同一批生成函数，保证"界面导出与 CLI 导出一致"。

### 6. build.bat 增加新源文件

`build.bat` 编译命令追加 `src\pdf.c`。

### 7. 前端统一导出入口 + 反馈

`src/web/index.html`：

* 把 `<button id="btnReport">生成报告</button>` 替换为：

  ```html
  <select id="repFmt">
    <option value="html">HTML</option>
    <option value="pdf">PDF</option>
    <option value="xls">Excel (.xls)</option>
    <option value="csv">CSV</option>
    <option value="json">JSON</option>
  </select>
  <button id="btnReport">导出报告</button>
  <span id="expMsg"></span>
  ```

`src/web/app.js`：

* 替换 `btnReport` 逻辑为 `doExport()`：

  * 取 `repFmt.value`，`$("#expMsg")` 显示"正在导出…"。

  * `fetch("/api/v1/report?format="+fmt)` → 若 `!res.ok` 抛错 → 读 `blob()` → 用 `URL.createObjectURL` + `<a download="memopt_report.<ext>">` 触发下载 → 撤销 objectURL。

  * `catch` → `expMsg` 显示"导出失败：<原因>"并红色样式；成功 → 显示"导出成功"。

  * **用 around the final**，取消使用 `window.open`（避免新标签 + 无法给出字节下载反馈）。

* `boot()` 里绑定 `$("#btnReport").onclick = doExport;`（替换原 L222）。

## 关键决策

1. **格式与 MIME**：html=`text/html`，pdf=`application/pdf`，xls=`application/vnd.ms-excel`，csv=`text/csv; charset=utf-8`，json=`application/json`。均加 `Content-Disposition: attachment` 实现"下载"而非打开。
2. **统一入口**：单一 format 下拉 + 导出按钮，不破坏现有布局、不加新页面（符合用户"不要 12 个页面、不让自动弹窗"偏好）。
3. **PDF 中文**：内嵌 simhei.ttf **字形子集**；字体缺失时 ASCII 降级，数据完整性不因字体失败而丢失。
4. **反馈**：统一用页面内 `expMsg` 文案（不弹系统原生窗干扰），绿/红状态区分成功/失败。
5. **CLI 与 Web 一致**：共用 `report.c`/`pdf.c` 生成函数。

## 假定（Assumptions）

* `simhei.ttf` 存在于 `C:\Windows\Fonts`（已确认存在，约 9.7MB）。

* 目标机器均为 Windows（工具本身即 Windows 进程监控），字体路径合理。

* `extra_headers` 已由 `http_server.c` 写入响应，无需改服务器传输层；只需保证指针在发送前有效（用 `_Thread_local`）。

* PDF 表格用基础 `Tj` 文本流 + 字距文本命令，不引入绘图旋转等高级特性，保证纯 C 可实现。

## 验证步骤（Verification）

1. `.\build.bat` 编译通过，无新增致命错误。
2. 启动服务：curl 各格式 URL，校验：状态 200、`Content-Disposition: attachment`、`content_type` 正确。

   ```
   http://127.0.0.1:8080/api/v1/report?format=json|pdf|xls|csv|html
   ```
3. **格式可打开性验证**（用 `F:\Python314\python.exe`，仅解析校验，不装库）：

   * JSON：`json.load` 成功且含 `system/hotspots/leaks/suggestions/processes` 键。

   * CSV：首行为表头、行数与 `sample_count` 一致。

   * xls：文件以 `<html`/`<meta` 开头且含 `<table`，`utf-8` BOM 存在。

   * PDF：文件头 `%PDF-`, 含 `/FontFile2` 与 `%%EOF`，子集字形存在；再用 PDF 查看可手动确认。
4. **完整性/一致性**：对比 `report?format=json` 的 `hotspots/leaks/suggestions` 数量与页面内 `/api/v1/hotspots`、`/leaks`、`/suggestions` 返回一致（确保"导出=界面显示"）。
5. **错误处理**：`format=xxx`（未知）→ 400 JSON；服务在无 simhei.ttf 环境（临时改 conf 指向不存在路径）→ PDF 仍输出 ASCII 版，不报 500。
6. **前端反馈**：浏览器点"导出 PDF/Excel/JSON"，页面显示"正在导出…"→"导出成功"，文件按名下载；故意传错 fmt 验证"导出失败"提示。
7. **CLI**：`--report json / pdf / xls` 均输出非空内容。
8. **回归**：原有 html/csv 导出仍正常；`--demo` + 一键优化/保护名单功能不受影响。

