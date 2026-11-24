# 构建、打包与安装

版本：2026-11-24。所有命令在 `ksu-native-design/` 目录内执行，产物只写入 `build/`、`module/bin/`、`module/zygisk/`。

## 1. 依赖

| 依赖 | 说明 |
| --- | --- |
| Android NDK r30 | 默认路径 `D:\BYBUS\android-ndk-r30`，可用环境变量 `BYBUS_NDK` 覆盖 |
| Node.js ≥ 20.19 或 ≥ 22.12 | 构建 Vite WebUI 与运行 `build/package.mjs` |
| PowerShell 5.1 | 运行 `build/build.ps1` |
| 主机 C++ 编译器（可选） | 跑 `tests/host_smoke.cpp`；本机通过 `D:\MarkNext\.marknext-dev\env.ps1` 提供 MSVC，`BYBUS_HOST_ENV` 可覆盖 |

不需要 cmake、gradle 或 Android SDK。

## 2. 构建

```powershell
.\build\build.ps1 -Target clean      # 清空 build/obj 与 build/out
.\build\build.ps1 -Target all        # 三个产物
.\build\build.ps1 -Target zygisk     # 只出 zygisk .so
.\build\build.ps1 -Target tools      # 只出 bybusd / bybusctl
```

产物：

| 文件 | 目标 | 说明 |
| --- | --- | --- |
| `build/out/bybus-zygisk.so` | arm64-v8a | 安装为 `zygisk/arm64-v8a.so`，导出 `zygisk_module_entry` / `zygisk_companion_entry` |
| `build/out/bybusd` | arm64-v8a | 后台守护进程 |
| `build/out/bybusctl` | arm64-v8a | WebUI 唯一入口 |

自研代码以 `-Wall -Wextra -Werror` 编译；`third_party/` 以 `-w` 编译。产物只依赖 `liblog/libz/libdl/libm/libc`，静态链接 libc++，不要求设备存在 `libc++_shared.so`。

`src/runtime` 中有 8 个只含实现的头文件（`rich_text.h`、`font_proof.h`、`flow_text_proof.h`、`managed_state.h`、`font_materials.h`、`font_scale.h`、`text_provenance.h`、`method_flags.h`）不在 `build/build.ps1` 的编译单元列表里：它们还没有被任何 `.cpp` 引用，本轮只单独做过 arm64 编译验证。接入 Hook 时需要同时把它们加入对应的编译分组，否则改动不会进入产物。

## 2.1 WebUI 构建（Vite）

```powershell
cd webui
pnpm install --config.strictDepBuilds=false
pnpm build
```

- 源文件在 `webui/`（`index.html`、`app.js`、`app.css`），官方 `kernelsu@3.3.0` 作为普通依赖引入；页面只调用 `kernelsu.exec`。
- 产物直接生成到 `module/webroot/`：`index.html`、`app.js`、`app.css`、`NOTICE.txt`（`webui/public/` 会被原样复制）。
- 构建配置固定在 `webui/vite.config.mjs`：`base: "./"`、固定文件名（无哈希）、IIFE 输出、并把 `index.html` 里的 `<script type="module">` 改回经典 `<script>`，兼容不同 KernelSU 管理器的 WebUI 加载方式。
- `pnpm install` 默认会拦下 esbuild 的 postinstall（`ERR_PNPM_IGNORED_BUILDS`），上面的命令用 `--config.strictDepBuilds=false` 显式放行；Vite 只用它做打包，不参与模块运行。

## 3. 主机测试

```powershell
.\build\test-host.ps1
```

覆盖纯数据层：配对器（含重复 ID 消歧、字符串数组、EGOgift 能力指纹）、`releasedTexts` 字段映射、快照清单校验与拒绝路径、特殊文本保留、SHA-256 向量、JSON 数值类型身份，以及翻译索引的编译/二进制写入/读取往返（含魔数不符被拒）和含 `guards` 的 AppSealing 清单序列化往返。全部通过时输出 `all host checks passed`。

归档导入与 7z 解码属于 arm64 目标，只能在设备或 arm64 CI 上验证。

## 3.1 运行时生成的快照目录与索引

`bybusd` 在 `pairing` 阶段写临时目录，成功后才发布为 `generations/<generation>/`：

```text
generations/<generation>/
  manifest.json          # 含 index 与 profile 的完整清单
  tables/<sha256>.json   # 每张合并后的表
  index/index.bin        # schema 9 翻译索引（LZTI1 魔数），本轮新增
  font/ChineseFont.ttf   # 有字体时才写入
```

`index/index.bin` 由 `src/translation/index_compiler.cpp` 生成，内容对应清单里的 `index`：`schema=9`、`path="index/index.bin"`、`sha256`、`bytes`、`unique`、`terms`、`contextual`。索引在 `pairing` 阶段与表同时写出，写入后与 `index/`、`tables/` 目录一起 `fsync`；任一步失败都让整个构建失败并删除临时目录。旧 generation 没有 `index` 时仍可发布与加载，运行时按“无索引”降级（JSON 表查表保留，散文/术语查表关闭）。

companion 的 Hello 会为 `manifest.json`、字体（若存在）、`index/index.bin`（若清单声明）各开一个只读描述符并按序交付；第三个描述符缺失或校验不符时运行时拒绝整个快照（回执 `code=snapshot_invalid`，日志理由 `snapshot_index_missing` / `snapshot_index_invalid`）。

## 4. 打包

```powershell
node build/package.mjs
```

- 先从 `build/out/` 复制 `bybusd`、`bybusctl`、`bybus-zygisk.so` 到 `module/`
- 校验必需文件存在（含顶层 `customize.sh`、`NOTICE`）
- 写入 ZIP 时保留 Unix 权限；KernelSU 安装器调用 `customize.sh` 对 `bybusd` 和 `bybusctl` 设置 0755，并自行设置 WebUI 的权限与 SELinux 上下文
- 写完立刻用**独立实现**复读归档：逐条目 inflate、比较字节数与 CRC-32（`node:zlib` 的 `crc32`），任一不符直接让打包失败。KernelSU 的 `ksud` 用 Rust `zip` 读包，CRC 不对会报 `i/o error: Invalid checksum`
- 输出三样东西：

| 文件 | 用途 |
| --- | --- |
| `build/bybus-<version>.zip` | 安装包 |
| `build/bybus-<version>.zip.sha256` | ZIP 自身的摘要，`sha256sum -c` 可直接校验传输完整性 |
| `build/SHA256SUMS` | 包内文件摘要，路径相对模块根（在 `module/` 下执行 `sha256sum -c` 才有意义） |

- 不打包源码、测试、`tools/` 或 `build/` 本身。

## 5. 安装

1. 通过 KernelSU 管理器安装 `build/bybus-1.0.0.zip`。
   管理器使用 KernelSU 安装器解包并运行顶层 `customize.sh`，也可用 `ksud module install bybus-1.0.0.zip`。
2. 确认已有可用的 Zygisk 实现（本模块不打包 ZygiskNext）；缺失时 `status.zygisk.code=zygisk_missing`。
3. 重启设备，或手动执行 `sh /data/adb/modules/bybus/service.sh` 启动 `bybusd`。
4. 打开模块 WebUI，先确认五件事：`game.textState=ready`、`game.profileState=ready`（或按设计可见的 `unsupported` 及其 `profileCode`）、`zygisk.available=true`、`snapshot.state`，以及诊断页的 `runtime.compatState`。

构建期还有一道安装资格检查：`bybusd` 从本机 APK 读取 SDK、页大小、ABI、签名证书 SHA-256 与 `<application android:name>`，只要 SDK < 26、ABI 不是 arm64-v8a、任一 `PT_LOAD` 对齐不合格或签名不是官方证书，就返回 `unsupported_runtime` 并拒绝发布快照（`job status` 的 `detail` 指明是 `qualification_sdk`、`qualification_abi`、`qualification_native_*` 还是 `qualification_signer`）。这类失败不能通过换汉化包绕过，只能换匹配的设备/安装包。


首次使用流程：`update start`（默认源）、`update start` 带 `mode:"path"` 指定本地包、或粘贴 HTTPS 地址 → 等待任务 `done` → 关闭游戏进程 → 重新启动游戏。

本地包不需要上传：WebUI 只把绝对路径交给后台，`bybusd` 以 root 直接读取并复制。在线官方源更新可在 WebUI 中填写 GitHub PAT，缓解 `api.github.com` 的匿名请求限额；点击“保存 PAT”后，下次打开会自动填入，“清除 PAT”会删除保存记录。PAT 只用于页面请求公开 Release 元数据，归档仍由后台下载。公开查询无需额外仓库权限，建议使用无 scopes 的专用 classic PAT。命令行等价写法：

```sh
/data/adb/modules/bybus/bin/bybusctl update start --json '{"mode":"path","path":"/sdcard/Download/LimbusLocalize_1.0.7z"}'
```

## 6. 卸载与数据

`uninstall.sh` 只停止守护进程、删除 socket 与 `run/`。**导入包、快照、来源表保留**在 `/data/adb/bybus/`，重装不丢数据。需要彻底清除时先创建标记：

```sh
touch /data/adb/bybus/.purge-on-uninstall
```

再卸载模块。

## 7. 故障排查

| 现象 | 命令 | 说明 |
| --- | --- | --- |
| WebUI 无响应 | `/data/adb/modules/bybus/bin/bybusctl status` | 直接看 JSON 与 `code` |
| 守护进程没起来 | `cat /data/adb/bybus/logs/bybusd.log` | 只含阶段与错误码 |
| 任务卡住 | `bybusctl job status --json '{"id":"j-..."}'` | 终态：done/failed/cancelled |
| 运行时不生效 | `bybusctl diagnostics` | 看 `runtime.hookState`、`fontState`、`compatState`、`profileState` |
| 构建被拒且 `detail` 是 `qualification_*` | `bybusctl job status --json '{"id":"j-..."}'` | SDK/ABI/PT_LOAD 对齐/签名资格不合格，换匹配的安装包或设备 |
| 构建被拒且 `detail` 形如 `build_index_*` | 同上 | 索引编译或写出失败；检查 `LZTI1` 写入、`index/` 目录权限与磁盘空间 |
| 游戏内散文/术语未汉化，但 JSON 表正常 | 进程日志里的 `snapshot rejected: snapshot_index_*` | 索引描述符缺失或校验不符；重新构建快照，确认 `manifest.json` 的 `index` 与 `index/index.bin` 一致 |
| 游戏启动被降级 | `cat /data/adb/bybus/runtime/degrade.json` | 仅 `{"generation":"<当前 generation>","reason":"manual"}` 的显式标记会跳过 Hook；旧格式的自动降级标记不再生效 |
| 管理器报 invalid checksum / 格式错误 | `sha256sum -c bybus-1.0.0.zip.sha256`；`unzip -l bybus-1.0.0.zip`；`tar -tvf bybus-1.0.0.zip` | 先确认安装包完整；包根目录必须有 `module.prop` 与 `customize.sh` |

## 8. 与 KernelSU 模块配置系统的关系

KernelSU 提供 `/data/adb/ksu/module_configs/<id>/`（`persist.config` / `tmp.config`，单个值 ≤1 MB、每模块 ≤32 项、键需匹配 `^[a-zA-Z][a-zA-Z0-9._-]+$`）。本模块**不使用**它：

- 状态包含来源表、任务、代次指针、归档摘要等结构化数据，且要求原子发布与并发串行化，用文件目录比键值配置更直接；
- `/data/adb/bybus/` 独立于模块目录，升级与禁用都不影响已发布快照；
- WebUI 只通过 `bybusctl` 读取状态，不依赖 `ksud`。

若希望管理器模块列表显示运行状态，可另行（可选）由 `bybusd` 调用 `ksud module config set override.description "…"`；该调用属于可选增强，缺失 `ksud` 时模块功能不受影响。
