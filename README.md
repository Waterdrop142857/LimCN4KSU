# LimCN4KSU

BYBUS 的 KernelSU 原生移植：在官方 `com.ProjectMoon.LimbusCompany` 安装包上，通过 Zygisk 模块完成本地化快照读取、内存文本替换、字体准备与 AppSealing 兼容注入。

## 结构

| 目录 | 说明 |
| --- | --- |
| `src/shared` | JSON、SHA-256、UTF 编解码、协议码、文件工具 |
| `src/translation` | 归档导入、JSON 配对、文本策略、快照构建、索引编译（`index_compiler`）、AppSealing 计划 |
| `src/service` | `bybusd` 后台任务、来源、代次、导入、诊断；APK 资格解析（签名、AXML、PT_LOAD 对齐） |
| `src/control` | `bybusctl` 控制入口 |
| `src/runtime` | Zygisk 模块、IL2CPP 绑定、Hook、索引读取与文本路由、字体、AppSealing 注入 |
| `webui` | Vite 构建的 KernelSU WebUI 源 |
| `module` | 模块安装包骨架（WebUI、脚本、字体、Zygisk/可执行文件由构建生成） |
| `tests` | 主机纯数据测试 |
| `third_party` | LZMA SDK、Zygisk API、Substrate |

## 功能

| 能力 | 状态 | 说明 |
| --- | --- | --- |
| 归档导入与配对 | 已实现 | ZIP/7z 签名识别、JSON 配对、快照原子发布 |
| 翻译索引 | 已实现 | `TranslationIndexCompiler.kt` 的 unique/terms/contextual 移植；schema 9（`LZTI1\0`）二进制写入 `index/index.bin` |
| 文本路由 | 已实现 | JSON 表 → UI 状态标签 → contextual → unique → terms → 原文 |
| 快照交付 | 已实现 | companion 通过 SCM_RIGHTS 交付 manifest/font/index 三个 fd |
| 文本 Hook | 已实现 | `LoadLocalizeFileSync`、`TextAsset.get_text`、UGUI `set_text`、TMP `set_text`/`set_text_string`、`Announcer.SetDescription`、`StorySystem.Dialog..ctor`、RPG 对话、选项、通知、`ActionChoiceButton`，以及通用对象字符串字段 |
| AppSealing 兼容 | 已实现 | 计划含 `guards`/`exactProfile`/`reportStable`；安装前逐字复核守卫 |
| APK 资格 | 已实现 | 签名证书 SHA-256、`<application android:name>`、PT_LOAD 对齐；不合格返回 `unsupported_runtime` |
| TMP 材质/字号/多语言字体 | 未接入 | `font_materials`/`font_scale` 已移植，未接 Hook |
| 富文本/注音 | 未接入 | `rich_text`/`font_proof`/`flow_text_proof`/`text_provenance` 已移植，未接 Hook |
| 剧情/RPG/选项/通知状态机 | 部分 | 仅通用字符串字段/参数翻译，layout/flow/choice 状态机未移植 |
| `android_dlopen_ext` watcher | 未实现 | 仍为轮询 |
| SIGSEGV 守卫 | 未实现 | 评估为 VirtualApp `MapsRedirector` 误报防护，本模块无该路径 |
| 应用级管理（coverage/source 备份/图标/更新链接） | 未实现 | 设计排除 |
| AI/容器/自更新/计费/迁移 | 未实现 | 设计排除 |

## 构建

需要 Android NDK r30、Node.js ≥ 20、PowerShell 5.1。

```powershell
.\build\build.ps1 -Target all
.\build\test-host.ps1
node .\build\package.mjs
```

产物：

- `build/out/bybus-zygisk.so`
- `build/out/bybusd`
- `build/out/bybusctl`
- `build/bybus-1.0.0.zip`

## 安装

1. 通过 KernelSU 管理器安装 `build/bybus-1.0.0.zip`。
2. 确认设备已有 Zygisk 实现（模块不打包 ZygiskNext）。
3. 重启或执行 `sh /data/adb/modules/bybus/service.sh`。
4. 打开 WebUI，检查游戏、Zygisk、快照、`profileState` 与 `profileCode`。
5. 导入/更新汉化包，等待任务完成，关闭并重新启动游戏。

## 文档

- [实现设计](01-implementation-design.md)
- [代码质量规范](02-code-quality-standard.md)
- [移植计划](10-port-plan.md)
- [AppSealing 兼容](12-kernelsu-appsealing.md)
- [控制协议](20-protocol.md)
- [构建与安装](30-build-and-install.md)
- [验证矩阵](40-validation-matrix.md)
- [代码清查](50-audit-findings.md)
- [第三方声明](THIRD_PARTY-NOTICES.md)

## 状态

- 纯数据层、arm64 交叉编译、模块打包与主机测试已通过；`tests/host_smoke.cpp` 覆盖索引编译/读取往返与含 `index` 的清单校验。
- L0–L3 实机矩阵仍为未验证：本目录环境没有 arm64 设备，因此没有任何“实机通过”的结论。AppSealing 的实际检查时机与游戏反馈需要按版本记录。
- `src/runtime` 下 `rich_text.h`、`font_proof.h`、`flow_text_proof.h`、`managed_state.h`、`font_materials.h`、`font_scale.h`、`text_provenance.h`、`method_flags.h` 为上游逐字节移植，已单独做 arm64 编译，但未进入 `build/build.ps1` 的编译单元，也尚未接入任何 Hook。
- AI 生成、VirtualApp 容器、游戏安装/更新、自更新与计费不在本模块范围。

## 许可

第三方组件许可见 `THIRD_PARTY-NOTICES.md` 与各 `VENDOR-RECORD.txt`。
