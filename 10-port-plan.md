# BYBUS → KernelSU 原生模块移植实施方案

版本：2026-11-23。基准：`D:\BYBUS\source` 提交 `bd32608`。规范来源：`01-implementation-design.md`、`02-code-quality-standard.md`。本目录独立于原仓库，产物不写入 `ksu-native-design/` 之外。

## 1. 运行边界与目录映射

| 设计规范板块 | 本实现目录 | 说明 |
| --- | --- | --- |
| `webui` | `webui/` 源 → `module/webroot/` 构建产物 | Vite + 官方 `kernelsu` 库；页面只调用 `kernelsu.exec` |
| `control` | `src/control/` | `bybusctl`：参数校验、IPC 客户端、固定 JSON 响应 |
| `service` | `src/service/` | `bybusd`：任务队列、代次发布、原文发现、字体准备 |
| `translation` | `src/translation/` | 纯数据：归档导入、配对器、快照构建 |
| `runtime` | `src/runtime/` | Zygisk 模块、IL2CPP 发现与 Hook、快照查找、字体 |
| `shared` | `src/shared/` | JSON、SHA-256、UTF 编解码、协议码、文件工具 |
| 第三方 | `third_party/{lzma-sdk,zygisk,substrate}` | 见 `THIRD_PARTY-NOTICES.md` 与各自 `VENDOR-RECORD.txt` |

模块安装后的结构（打包产物）：

```text
module/
  module.prop
  customize.sh
  service.sh
  post-fs-data.sh
  action.sh
  uninstall.sh
  webroot/
    index.html
    app.js
    app.css
  bin/
    bybusctl
    bybusd
  zygisk/
    arm64-v8a.so
  assets/
    ChineseFont.ttf
    font.sha256
    Sarasa-Gothic-OFL-1.1.txt
```

私有状态根：`/data/adb/bybus/`（模块安装目录之外）。

```text
/data/adb/bybus/
  state.json                 # schema、选中源、最近构建状态
  sources/<source>.json      # 源登记（id、名称、归档摘要、表数）
  archives/<sha256>.pack     # 已验证的原始归档，只读保留
  imports/<session>/         # 未提交的导入会话（可清理）
  generations/<gen>/         # 不可变快照
    manifest.json
    tables/<sha256>.json
    font/ChineseFont.ttf
  active                     # 原子切换的快照指针（内容为 generation id）
  jobs/<job>.json            # 任务持久状态
  logs/diagnostics.log       # 有界诊断，只含阶段/计数/错误码
  runtime/                   # receipt.json、degrade.json（由 companion 写入）
  bybus.sock                 # bybusctl ↔ bybusd 的 Unix 域套接字（0600）
  inbox/                     # 用户投放的 7z/ZIP，`update start --inbox` 消费
```

## 2. 与原实现的对应关系

| 原实现 | 本实现 | 移植要求 |
| --- | --- | --- |
| `container/TranslationJsonPairer.kt` | `src/translation/json_pairer.cpp` | 逐字段等价：稳定 ID、重复 ID 消歧、嵌套数组、技能/罪恶共鸣/事件/指南/RPG 对话作用域、贪心剩余配对 |
| `container/TranslationTextPolicy.kt` | `src/translation/text_policy.cpp` | 显示字段白名单与 `STRING_ARRAY_FIELDS`；不移植内容质量筛选 |
| `container/TranslationSnapshot.kt` | `src/translation/json_pairer.cpp`（快照绑定部分） | 标记 `_bybusTextSnapshot`、编码/解码、`matchingStructure`（数值按十进制比较） |
| `translation/LocalTranslationArchive.kt` | `src/translation/archive_import.cpp` | ZIP/7z 签名识别、安全路径、重复目标、`mapResource` 映射；不移植数量与内容大小上限 |
| `update/TranslationUpdateManager.kt` | `src/service/source_fetch.cpp` | 固定仓库 `LocalizeLimbusCompany/LocalizeLimbusCompany` 的最新 Release，资产名 `LimbusLocalize_<数字>.7z` |
| `third_party/.../LimbusTranslationRuntime.cpp` | `src/runtime/` | 只保留 IL2CPP 发现、`TextAssetPatchManager.LoadLocalizeFileSync` / `TextAsset.get_text` 读后合并、字体准备；移除 VirtualCore/JNI 回调/容器重定向 |
| `.../Foundation/LimbusCodeIntegrityCompat.cpp`（报告桥与站点补丁） | `src/runtime/appsealing.cpp`、`src/runtime/appsealing.h` | 移植 JNI detach 兼容点、50048 报告桥、70034/50040 站点补丁、471 瞬时 50048 维护与 SIGILL 守卫；偏移、期望指令字与栈布局全部来自本机清单，不写死版本常量 |
| `.../Foundation/LimbusOwnedCode.h` | `src/runtime/owned_code.h` | 保留自有代码段的范围表示，只用于按清单重算范围 CRC 与指纹；账本同时覆盖 `libil2cpp.so` 与 `libcovault-appsec.so` |
| `.../Foundation/IORelocator64.cpp`（AppSealing 部分） | `src/translation/appsealing_profile.cpp`、`src/translation/appsealing_profile.h` | 只移植站点资格判定与计划生成（`profile.appsealing`）；容器 I/O 重定向、`MapsRedirector` 与 `syscall` Hook 不移植 |
| `And64InlineHook.cpp`、`SubstrateHook.cpp` | `third_party/substrate/` | 原文件与 MIT/BSD 声明一并移植 |

## 3. 与设计文档的已知偏差

1. **字体资产格式**：设计列 `assets/ChineseFont.ttf.xz`。本构建环境无 xz 编码器，模块 ZIP 本身已使用 deflate，故以 `assets/ChineseFont.ttf` 原样分发（SIL OFL 1.1，来源 `LocalizeLimbusCompany/Fonts/ChineseFont.ttf`），`bybusd` 构建快照时校验固定 SHA-256，再复制进 generation。压缩方式变更不改变校验语义。
2. **在线下载**：`bybusd` 不内嵌 TLS 栈。下载阶段委派给设备上可用的结构化取回器（`/system/bin/curl`，缺失时回退 `wget`/`/vendor/bin/curl`），argv 全为固定字面量与已校验的 URL，不经 shell；都不可用时任务以 `network_unavailable` 结束，并提示改用 `inbox/` 投放或 `import` 导入。
3. **`sources select` 之外的源管理**：首版只提供 `zero` 默认源与本地导入源（`local-<sha256 前 16 位>`）；AI 生成、撤回、图标、更新链接不移植。
4. **安装入口**：使用 KernelSU 自带安装器；顶层 `customize.sh` 只设置可执行文件权限，不写 `/data/adb/bybus`，也不更改 WebUI 的权限与 SELinux 上下文。

## 4. 关键契约

- 快照条目同时绑定：源 id、归档 SHA-256、游戏安装标识、规范资源名、原文 UTF-8 摘要、合并结果摘要与字节长度。运行时按 `(资源名, 原文摘要)` 命中，文件名单独命中不替换。
- 一个 generation 发布后只读；`active` 指针以 `rename` 原子切换；中断只留临时目录，启动时清理 `imports/`、`generations/.tmp-*`。
- 游戏进程启动时固定一个 generation；后台发布的新 generation 只服务下一次启动。
- 游戏主线程不做网络、解包、全量配对；运行时只用启动期载入的索引与每表一次性摘要查找。
- 诊断只含阶段、计数、版本摘要与受限错误码；不写原文、译文、账号、设备标识。

## 5. 开发顺序

1. `shared` + `translation` 纯数据层与桌面测试夹具。
2. `service`（任务、代次、原文发现、字体）与 `control`。
3. WebUI 静态页面。
4. `runtime`：空 Zygisk 模块 → `LoadLocalizeFileSync` 读后合并 → 字体 → 单表中文显示。
5. 打包、校验、安装/禁用/卸载回归。

## 6. 未在设备上验证的未知

- 游戏原文目录与表名清单（`Localize/jp|en|kr/...`）需按实际安装确认。
- 加载器是否改变 JSON 字节表示（BOM、换行、预处理）需实测；不一致时该表回退原文并写诊断。
- 设备上由 root 守护进程读取 `/sdcard/Download/...` 的实际访问权限；失败时改用 `inbox/` 或其他可读绝对路径。
- 注入后的游戏保护反馈需按版本分别记录：AppSealing 兼容注入（detach 兼容点、50048 报告桥、70034/50040 站点、471 瞬时维护、SIGILL 守卫）已移植，但实际检查时机与反应仍要实机确认，见 `12-kernelsu-appsealing.md` 与 `40-validation-matrix.md` 的 L0–L3。
- `/proc/self/fd/<n>` 作为 Unity 字体路径是否被接受；不接受时使用辅助目录副本。
