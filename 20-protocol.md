# bybusctl / bybusd 控制面协议

版本：schema 1。适用范围：`module/webroot/app.js`（WebUI）、`bin/bybusctl`、`bin/bybusd`。三者共享本文件；既有字段出现不兼容变化时必须提升 `schema`，可选参数可向后兼容地添加。

## 1. 传输

- WebUI 通过 KernelSU 的 `ksu.exec` 调用 `/data/adb/modules/bybus/bin/bybusctl`，读取其标准输出的单个 JSON 对象。
- `bybusctl` 的 argv 固定为 `<command tokens...> [--json '<args object>']`：命令词按空格拆成多个 argv 元素（如 `sources select --json '{"id":"zero"}'`），`--json` 后只跟一个 JSON 对象参数。省缺 `--json` 表示空 `args`。
- `bybusctl` 连接 `/data/adb/bybus/bybus.sock`，写入一行 UTF-8 JSON 请求（≤ 256 KiB，含换行），读取一行 JSON 响应（≤ 4 MiB）。没有换行、超长、或超时（默认 30 s，`update start` 与 `snapshot build` 为 10 s 受理超时）即视为传输失败。
- `bybusctl` 对 WebUI 永远输出一个 JSON 对象并返回退出码 0，`ok=false` 时在同一对象内给出错误码。只有参数无法解析时才返回 `usage`。
- 请求形状：`{"schema":1,"command":"<name>","args":{...}}`。
- 响应形状：`{"schema":1,"ok":true,"code":"ok","message":"","data":{...}}`；失败时 `ok=false`，`code` 取 §4 的固定错误码，`message` 是简短中文说明，不含路径以外的自由文本。

## 2. 命令

| 命令 | `args` | `data` |
| --- | --- | --- |
| `status` | `{}` | 见 §3.1 |
| `sources list` | `{}` | `{"sources":[Source...],"selectedId":"..."}` |
| `sources select` | `{"id":"<sourceId>"}` | `{"selectedId":"...","effective":"next_launch"}` |
| `sources remove` | `{"id":"<sourceId>"}` | `{"sources":[...],"selectedId":"..."}` |
| `update start` | `{"mode":"release"}`（可附 `assetUrl`、`expectedSha256`）或 `{"mode":"inbox"}` 或 `{"mode":"url","url":"https://..."}` 或 `{"mode":"path","path":"/sdcard/Download/LimbusLocalize_1.0.7z"}` | `{"jobId":"...","state":"queued"}` |
| `import begin` | `{"name":"<文件名>","totalBytes":<int>,"sha256":"<64 hex>"}` | `{"sessionId":"...","chunkBytes":262144}` |
| `import chunk` | `{"sessionId":"...","index":<int>,"base64":"..."}` | `{"received":<int>,"total":<int>}` |
| `import commit` | `{"sessionId":"...","name":"<显示名>","activate":<bool>}` | `{"jobId":"...","sourceId":"..."}` |
| `import abort` | `{"sessionId":"..."}` | `{}` |
| `snapshot build` | `{"sourceId":"<id>"}` | `{"jobId":"...","state":"queued"}` |
| `job status` | `{"id":"<jobId>"}` | 见 §3.3 |
| `job cancel` | `{"id":"<jobId>"}` | `{"id":"...","state":"cancelled"}` |
| `diagnostics` | `{}` | 见 §3.4 |
| `runtime probe` | `{}` | `{"module":..., "zygisk":..., "game":...}`（与 `status` 同源） |
| `uninstall cleanup` | `{"confirm":true}` | `{"removed":[...]}` |
| `shutdown` | `{}` | `{"state":"shutting_down"}`（仅供 `uninstall.sh` 使用，WebUI 不得调用） |

固定拒绝条件：未知命令 → `unknown_command`；字段缺失或类型不符 → `invalid_argument`；`sources select` 指向不存在的源 → `source_missing`；`import chunk` 的 `index` 越界、`base64` 超过 262144 解码字节 → `invalid_argument`；`uninstall cleanup` 无 `confirm` → `invalid_argument`。

## 2.1 `mode:"path"`（推荐的本地导入方式）

`bybusd` 以 root 运行，因此本地包不需要经 WebUI 上传：WebUI 只传路径，后台直接读取并复制。

- `path` 必须是绝对路径（`/` 开头）、长度 ≤ 4096、不含 `..` 路径段与控制字符，否则 `invalid_argument`。归档格式根据文件签名识别，不要求后缀。
- 文件不存在或不是普通文件时返回 I/O 错误；归档内容没有预设大小、条目数或文本长度上限。
- 复制阶段沿用 `fetching` 阶段名，随后 `validating` → `pairing` → `publishing`。
- 校验通过后归档以内容摘要存为 `archives/<sha256>.pack`，并登记 `local-<摘要前16位>` 源；该源**自动被选中**（对运行中的游戏下一次启动生效）。
- `import begin/chunk/commit` 是旧命令行兼容接口；WebUI 使用 `mode:"path"`，不传输包内容。

## 2.2 使用 PAT 查询官方 Release

WebUI 的 PAT 输入框可按用户选择保存到 KernelSU 管理器的 WebView `localStorage`，下次打开自动填入；“清除 PAT”同时删除输入框内容与保存记录。公开 Release 查询不要求额外仓库权限，专用的无 scopes classic PAT 即可。填写后，页面向固定的 `https://api.github.com/repos/LocalizeLimbusCompany/LocalizeLimbusCompany/releases/latest` 发起带 `Authorization: Bearer` 的请求，选出 `LimbusLocalize_<数字>.7z` 与 API 返回的 SHA-256 摘要，然后向 `bybusctl` 提交 `update start`：

KernelSU 当前管理器将模块 WebUI 放在同一个 `mui.kernelsu.org` 来源下；同源页面可能读取此 `localStorage`。因此只建议保存专用、无 scopes 的 PAT，不把此存储视为跨模块秘密隔离。

```json
{"mode":"release","assetUrl":"https://github.com/LocalizeLimbusCompany/LocalizeLimbusCompany/releases/download/<tag>/LimbusLocalize_<数字>.7z","expectedSha256":"<64 hex>"}
```

`bybusd` 只接受该固定仓库的直接 Release 资源 URL，并在导入前核对归档摘要。PAT 不进入控制协议、进程命令行、下载 URL、任务记录或日志；仅在用户点击“保存 PAT”后进入 WebView 本地存储。输入框留空时，后台沿用不带 PAT 的公开 API 请求。

## 3. 数据形状

### 3.1 `status`

```json
{
  "schema": 1,
  "module": { "id": "bybus", "version": "1.0.0", "versionCode": 1, "state": "enabled" },
  "zygisk": { "available": false, "provider": "none", "code": "zygisk_missing" },
  "game": {
    "installed": true,
    "packageName": "com.ProjectMoon.LimbusCompany",
    "versionCode": 0,
    "versionName": "",
    "installIdentity": "<64 hex>",
    "textState": "ready",
    "textCode": "ok",
    "locales": ["jp"],
    "tableCount": 0,
    "profileState": "ready",
    "profileCode": "ok"
  },
  "snapshot": {
    "state": "ready",
    "generation": "<64 hex>",
    "sourceId": "zero",
    "builtAtMillis": 0,
    "tableCount": 0,
    "runtimeSchema": 1,
    "code": "ok"
  },
  "source": { "id": "zero", "name": "都市零协会", "kind": "official", "revision": "", "tableCount": 0, "pendingActivation": false },
  "job": { "id": "", "state": "idle", "stage": "", "done": 0, "total": 0, "code": "ok" },
  "storage": { "usedBytes": 0, "limitBytes": 0 },
  "code": "ok"
}
```

`game.textState` ∈ `ready` / `waiting_for_game_text` / `game_missing`。
`game.profileState` ∈ `ready` / `missing` / `unsupported` / `invalid`：`ready` 表示当前 generation 的清单带有运行时可用（且对声明了保护库的版本已给出 AppSealing 计划）的 profile；`missing` 表示快照没有 profile；`unsupported` 表示 APK 声明了保护库但清单里没有合格计划，运行时不会安装任何 Hook，`profileCode=unsupported_runtime`；`invalid` 表示清单读取或解析失败。
`snapshot.state` ∈ `missing` / `building` / `ready` / `invalid`。
`zygisk.provider` ∈ `zygisknext` / `none`；不可用时 `code=zygisk_missing`。
`source.pendingActivation=true` 表示选中源与已发布快照的源不同，UI 必须显示“下次启动生效”。

### 3.2 `Source`

```json
{ "id": "zero", "name": "都市零协会", "kind": "official|local", "revision": "<64 hex>", "tableCount": 0, "archiveSha256": "<64 hex>", "updatedAtMillis": 0 }
```

### 3.3 `job status`

```json
{ "id": "j-<16 hex>", "state": "queued|fetching|validating|pairing|publishing|done|failed|cancelled",
  "stage": "validating", "done": 0, "total": 0, "code": "ok", "errorCode": "", "startedAtMillis": 0,
  "progress": { "phase": "extract", "bytesDone": 0, "bytesTotal": 0 } }
```

`state=done` 且 `errorCode=""` 表示快照已发布；`state=failed` 时 `errorCode` 取 §4。

### 3.4 `diagnostics`

```json
{ "schema": 1, "runtime": { "generation": "<64 hex>", "hits": 0, "misses": 0, "fontState": "unknown", "hookState": "unknown", "compatState": "unknown", "code": "ok" },
  "build": { "lastGeneration": "", "lastCode": "", "generations": 1, "discarded": 0, "profileState": "ready", "profileCode": "ok" },
  "log": [ { "atMillis": 0, "stage": "pairing", "code": "ok", "count": 0 } ] }
```

`log` 最多 200 条；只含阶段、计数与错误码。

`diagnostics.runtime` 的字段来自 companion 回执 `/data/adb/bybus/runtime/receipt.json`，与回执一一对应：

| 字段 | 取值 | 说明 |
| --- | --- | --- |
| `generation` | `<64 hex>` | 进程启动时固定的 generation |
| `hits` / `misses` | 计数 | 快照命中与未命中次数 |
| `fontState` | `unknown` / `none` / `ready` / `rejected` | 字体准备结果 |
| `hookState` | `unknown` / `none` / `installed` / `rejected` | 文本 Hook 安装结果 |
| `compatState` | `unknown` / `none` / `installed` / `rejected` | AppSealing 兼容注入结果：`none` 未安装，`installed` detach 兼容点、报告桥与站点补丁全部就位，`rejected` 安装失败并已回滚；`unknown` 表示尚无回执 |
| `code` | 固定错误码 | 该次启动的终止理由 |

`compatState` 与 `hookState` 相互独立：`compatState=installed` 不表示保护检查会通过，`hookState=installed` 也不代表兼容注入生效。回执（`diagnostics.runtime`）里没有 `message`、原文、译文、账号或设备标识。

`build.profileState` 与 `game.profileState` 同源同取值（`ready` / `missing` / `unsupported` / `invalid`），`build.profileCode` 同理取 `ok` / `unsupported_runtime` / `snapshot_invalid`。

## 4. 错误码（固定集合）

`ok`、`usage`、`unknown_command`、`invalid_argument`、`game_missing`、`text_missing`、`game_missing_text`、`archive_invalid`、`archive_limit`、`archive_path_unsafe`、`archive_duplicate`、`source_missing`、`source_changed`、`font_invalid`、`snapshot_invalid`、`snapshot_stale`、`snapshot_busy`、`unsupported_runtime`、`zygisk_missing`、`network_unavailable`、`network_rejected`、`job_missing`、`job_cancelled`、`io_error`、`internal_error`。

WebUI 只按该集合显示本地化文案，不解析 `message`。

## 5. 任务语义

- `update start`、`import commit`、`snapshot build` 立即返回 `jobId`；页面关闭后任务继续，重开页面用 `job status` 恢复。
- 同一源与同一归档摘要的并发构建串行化；重复请求复用正在运行的任务（返回同一 `jobId`）。
- `job cancel` 只对未进入 `publishing` 的任务生效；`publishing` 阶段不可取消。
- 构建中断（进程被杀、重启）后：`jobs/*.json` 中非终态任务在启动时标记 `failed`、`errorCode=io_error`，`imports/` 清空，最后一个 `generations/<gen>` 与 `active` 保持一致。

## 6. 关闭与恢复

WebUI `localStorage` 保存标签页偏好；用户明确点击“保存 PAT”后也保存该令牌。源选择、任务、快照结果全部来自 `status`，不落 `localStorage`。清除 KernelSU 管理器应用数据或卸载管理器会丢失保存的 PAT。
