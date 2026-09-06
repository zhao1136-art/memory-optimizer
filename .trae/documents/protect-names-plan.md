# 保护名单（受保护进程）功能实现计划

## 概述（Summary）

用户担心"一键优化 / 优化建议"会误关自己重要的程序。现状的受保护名单是**硬编码**在 `optimize_oneclick.c` 里的（约 17 个系统进程名 + `PID≤50`），用户**无法自定义**，且 `optimize_apply`（单项"执行"）**完全没有保护判断**。

本方案让保护名单变为**可配置、可持久化、可在 Web 界面增删**，并让**所有优化动作（一键优化 + 单项建议执行）一律跳过受保护进程**。

用户已确认的选择：

* **Web 界面可增删**（新增一个"受保护进程"面板，持久化到磁盘）

* **保护对所有优化动作一律生效**（一键优化、单项 reclaim 执行都跳过）

## 现状分析（Current State Analysis）

### 相关文件与逻辑

| 文件                                | 作用                                                                            |
| --------------------------------- | ----------------------------------------------------------------------------- |
| `src/optimize_oneclick.c` L13-L36 | 硬编码保护名单 `oc_protected_names[17]` + `oc_is_protected()`（含 `PID≤50` 判断），供一键优化跳过 |
| `src/optimize.c` L119-L143        | `optimize_apply()`：单项"执行"入口，目前**无任何保护判断**，直接 `SetProcessWorkingSetSize`       |
| `src/routes.c` L508-L555          | `api_apply`（POST /suggestions/apply）、`api_optimize`（POST /optimize）两个对外入口     |
| `src/routes.c` L608-L626          | 路由分发：通过 `t`（去掉 `/api/v1/`）精确匹配端点                                              |
| `src/web/index.html`              | 单页布局，`<main>` 内若干 `<section class="panel">`                                   |
| `src/web/app.js` L72-L87          | `renderSuggestions()`：渲染可执行的 `执行：reclaim` 按钮                                  |
| `src/web/app.js` L159-L180        | `doOneClick()`：一键优化 → POST /optimize，点击有 confirm                              |
| `include/memtool.h` L71-L84       | `memtool_config` 结构（配置项）                                                      |
| `src/config.c` S2-S20             | `[server][sampler][storage]` 三节配置解析                                           |

### 关键缺陷（本次要解决）

1. **保护名单写死**，用户不能把 `python.exe`、`gcc.exe`、自己的业务进程加入保护。
2. **单项"执行"完全无保护**：即便是一键优化被跳过的进程，用户在建议列表点"执行：reclaim"仍会被修剪工作集，违背用户"所有优化动作一律跳过"的诉求。
3. **无持久化**：即使加了名单，重启服务就丢失。
4. **无 Web 管理入口**：用户无法在界面上增删。

## 拟改动（Proposed Changes）

### 1. 新增保护名单持久化数据模块 `src/protect.c` + `include/protect.h`

负责：加载、保存、查询、增删保护名单。

**数据结构**（`memtool.h` 或 protect.h）：

```c
#define PROTECT_MAX 64
typedef struct protect_list {
    char names[PROTECT_MAX][96]; /* 小写进程名（含 .exe），去重 */
    int  count;
} protect_list;
```

**函数** `include/protect.h`：

```c
void protect_load(protect_list *pl, const char *dir);      /* 从 <dir>/protect.txt 读取 */
mt_err protect_save(const protect_list *pl, const char *dir);
int  protect_contains(const protect_list *pl, const char *name);      /* 大小写不敏感匹配 */
mt_err protect_add(protect_list *pl, const char *dir, const char *name);
mt_err protect_remove(protect_list *pl, const char *dir, const char *name);
/* 是否整体关闭"关进程"这个动作本身：由底层名单决定 */
```

* 文件格式：`<data_dir>/protect.txt`，每行一个小写进程名（如 `python.exe`），`#` 开头为注释。

* `protect_load` 在服务启动时调用，把默认系统名单并入（见第 3 点）。

### 2. 保护判定统一函数

把"是否受保护"统一为一个可复用的入口（供一键优化和单项 apply 共用）：

```c
int prot_is_protected(protect_list *pl, uint32_t pid, const char *name);
```

规则：

* `pid <= 50` → 保护（系统关键 PID 段）

* 进程名匹配内置系统名单（从 `oc_protected_names` 平移）→ 保护

* 进程名匹配用户 `protect.txt` 名单 → 保护

### 3. 重构 `optimize_oneclick.c`

* 删除 `oc_protected_names` 硬编码判定，改为接收外部 `protect_list *pl` 并调用 `prot_is_protected(pl, pid, name)`。

* 签名调整：`optimize_oneclick(s, pl, out, options)`。

* **默认内置名单**：在 `routes.c`/`main.c` 初始化时，若 `protect.txt` 为空，把系统名单写入作为默认。更简单：`protect_load` 现将硬编码系统名单作为初始内容灌入（运行时与文件内容合并，文件里删除的系统项不强制保留——保留内置即可）。

### 4. `optimize_apply` 增加保护判断

`src/optimize.c`：

* 签名改为 `optimize_apply(s, pl, pid, action, err, err_sz)`。

* 开头校验 `prot_is_protected(pl, pid, name)`——受保护则返回 `MT_ERR_PERMISSION`（错误信息"该进程受保护，已跳过"）。需要拿到 name：用 `counters_process(pid, &cur)` 或 `sampler` 查进程名；`api_apply` 已持有 sampler。

### 5. 路由：新增保护名单 CRUD API

`src/routes.c`：

* 新增 `api_protect_list(a)` → `GET /api/v1/protect`，返回 `{count, names:[...]}`（含内置标记可选）。

* 新增 `api_protect_add` → `POST /api/v1/protect`，body `{"name":"python.exe"}`。

* 新增 `api_protect_remove` → `DELETE /api/v1/protect`，body `{"name":"python.exe"}` 或 `POST` with `{"name":..., "remove":1}`（取决于 http 层是否支持 DELETE 方法）。

* 路由分发补三行（在 `routes.c` L608-L626 区域）：

  ```c
  else if (!strcmp(t, "protect")) r = api_protect_list_and_modify(a, req);
  ```

  为简化 HTTP 方法处理，可统一用 `POST /protect`（body 含 `op: list|add|remove`），POST 结构最稳，与既有 `suggestions/apply`、`optimize` 风格一致。

  * `POST /protect` body `{"op":"list"}`

  * `POST /protect` body `{"op":"add","name":"xxx"}`

  * `POST /protect` body `{"op":"remove","name":"xxx"}`

### 6. 前端：新增"受保护进程"面板

`src/web/index.html`：在 `<main>` 里"碎片分析"之后新增：

```html
<section class="panel">
  <h2>受保护进程（优化时绝不干预）</h2>
  <div id="protBox">
    <input id="protName" placeholder="进程名，如 python.exe" maxlength="96">
    <button id="btnProtAdd">加入保护</button>
    <div id="protList" class="list"></div>
  </div>
</section>
```

`src/web/app.js`：

* 新增 `renderProtect()`：`POST /protect {op:"list"}` → 渲染为每个名字一个"移除"按钮。

* 新增 `addProtect()`：读输入框 → `POST {op:"add",name}` → 刷新。

* 新增 `removeProtect(name)`：`POST {op:"remove",name}` → 刷新。

* `boot()` 里绑定 `btnProtAdd.onclick`、`refresh()` 里调用 `renderProtect()`。

* **一键优化 confirm 文案**追加一行"受保护进程将一律跳过"。

### 7. `app_ctx` 携带 protect\_list

`routes.c` 顶部 `app_ctx` 结构（含 `sampler/cfg/running/store`）增加 `protect_list prot;`。`main.c` 初始化时 `protect_load(&app.prot, cfg.data_dir)`。

## 关键决策

1. **API 风格统一用 POST /api/v1/protect**（树 JSON `op` 字段），避免依赖 http\_server 是否实现 `DELETE` 方法，与现有 POST 端点一致。
2. **内置系统名单保留且不可被删除**：把它们作为"总是受保护"层，用户名单作为追加层；Web 列表只展示用户可管理的部分 + 提示内置系统进程自动受保护。
3. **持久化文件**：`<data_dir>/protect.txt`，服务启动 `protect_load` 合并用户名单；内置名单不写入该文件（避免被删），仅运行时合并。
4. **保护粒度**：按进程名（含 .exe）匹配，大小写不敏感。不按 PID（PID 会变）。

## 假定（Assumptions）

* `http_server` 已支持 POST body 解析（既有 `suggestions/apply`、`optimize` 已用，成立）。

* 取消/无需单独 DELETE 方法；用 POST + op 字段。

* 不以管理员运行时，保护名单对读自身进程文件不受限（data\_dir 是程序可写目录）。

## 验证步骤（Verification）

1. `.\build.bat` 编译通过，`bin\memtool.exe` 生成。
2. 以 `--demo` 启动，`POST /protect {"op":"list"}` 返回内置系统名单。
3. `POST /protect {"op":"add","name":"python.exe"}` → list 中可见；确认 `data/protect.txt` 已写入。
4. `POST /optimize {}`：若 `python.exe` 受保护，其不会出现在 killed/reclaimed 明细。
5. 单项：对一受保护进程 `POST /suggestions/apply` → 返回"受保护，已跳过"，不执行 `SetProcessWorkingSetSize`。
6. 前端：新增"受保护进程"面板可增删；一键优化 confirm 含保护提示。
7. 重启服务后 `protect.txt` 名单仍在（持久化验证）。

