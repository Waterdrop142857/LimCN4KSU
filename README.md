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
| 归档导入与配对 | 已实现 | ZIP/7z 签名识别、JSON 配对、快照原子发布；配对覆盖率不足 90% 时快照仍发布但不激活索引 |
| 翻译索引 | 已实现 | `TranslationIndexCompiler.kt` 的 unique/terms/contextual 移植；二进制为参考布局：`LZTI1\0` + schema 9 + 三个计数，随后三组「`u32` 长度 + UTF-8」条目，按插入序写出，无尾部填充 |
| 文本路由 | 已实现 | JSON 表 → UI 状态标签 → contextual → unique → terms → 原文 |
| 文本质量门禁 | 已实现 | `is_blank`（Kotlin `String.isBlank()` 语义）与 `is_usable_display_text`（拒 U+FFFD、除 `\n\r\t` 外的 C0、`0x7F..0x9F`）；此类译文不进索引，也不会用空串覆盖原文 |
| 快照交付 | 已实现 | companion 通过 SCM_RIGHTS 交付 manifest/font/index 三个 fd；表缓存键为 `(generation, digest, bytes, 实际长度)` |
| 文本 Hook | 已实现 | `LoadLocalizeFileSync`、`TextAsset.get_text`、UGUI `set_text`、TMP `set_text`/`set_text_string`、`Announcer.SetDescription`、`StorySystem.Dialog..ctor`、`StoryData.GetScenario`、`RPGSystem.RpgLocFormat.Apply`、RPG 对话、选项（`SetText`/`SetTotalContent`/`ReadingContent`/`Clear`/`OnDisable`）、通知、`ActionChoiceButton`，以及按显示字段白名单翻译的通用对象字符串字段；仅两个入口钩子失败才回滚并回执 `unsupported_runtime`，其余钩子失败只记日志 |
| AppSealing 兼容 | 已实现 | 计划含 `guards`/`exactProfile`/`reportStable`；安装前逐字复核守卫，并重算 exactProfile 与清单比对；自有代码段审计只写日志，不阻断装钩 |
| APK 资格 | 已实现 | 签名证书 SHA-256、`<application android:name>`、PT_LOAD 对齐；不合格返回 `unsupported_runtime` |
| TMP 材质/字号/多语言字体 | 已接入 | `font_materials`/`font_scale` 已接入：按组件保存原字体与材质，切换中文回退字体、转换共享材质并保持 shader ratio；字体创建最多尝试 3 次；语言字体 owner 只保留通用兼容，无实机签名证据 |
| 富文本/注音 | 已接入 | `rich_text`/`font_proof`/`flow_text_proof`/`text_provenance` 已参与文本判定；`TMProStringMatcher` 六类格式化入口与三个 Ruby formatter 已接入，未验证实机调用频率 |
| 剧情/RPG/选项/通知状态机 | 部分 | 已按显示字段白名单翻译字符串，并保存/恢复 TMP wrapping/overflow；原版 layout/flow/choice 的完整组件级证明状态机仍未逐项移植 |
| `android_dlopen_ext` watcher | 已实现 | 通过 `android_dlopen_ext` Hook 等待 `libil2cpp.so`，保留有界轮询兜底 |
| SIGSEGV 守卫 | 已实现 | 471 回溯恢复、旧处理器链式调用、上下文变化检测与 AppSealing 重包裹；无实机验证 |
| 应用级管理（coverage/source 备份/图标/更新链接） | 未实现 | 设计排除 |
| AI/容器/自更新/计费/迁移 | 未实现 | 设计排除 |

## 构建

需要 Android 交叉编译工具链（NDK r30）与 Node.js ≥ 20。`build/` 下的脚本是 Windows 下编译用的入口；Linux 暂无等价入口。

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

- 纯数据层、arm64 交叉编译、模块打包与主机测试通过；`build/test-host.ps1` 编译 `tests/*.cpp` 全部，用例见 [构建与安装](30-build-and-install.md) §3。
- 本轮与参考实现对位：文本质量门禁、配对数值绑定与遍历上限、ID 边界、配对覆盖率门槛、索引参考布局与摘要/去重键、装钩失败语义（部分生效）、方法签名归一化、字体尝试预算、完整性审计口径、快照表缓存键、描述符生命周期、服务层边界。以上只经过桌面编译与主机测试，实机未验证。
- 索引二进制布局已改为参考布局，升级后需重建一次快照；L0–L3 实机仍未验证：目前没有任何 arm64 实机测试记录，因此没有任何“实机通过”的结论。AppSealing 的实际检查时机与游戏反馈需要按版本记录。
- `src/runtime` 下的字体/富文本/状态辅助头文件已由 `hooks.cpp` 引入并随模块编译；TMP 字体、共享材质、shader ratio、富文本格式化与 Ruby formatter 已接 Hook，但签名是否在真实构建中全部命中、调用频率与材质复用行为仍需实机确认。
- 当前 `exactProfile` 为必填：缺少、为空或与计划字段不一致的清单会被拒绝；旧 generation 需要重新构建。
- 应用级 coverage/source 备份/图标/更新链接、AI 生成、VirtualApp 容器、游戏安装/更新、自更新与计费不在本模块范围。

## 许可

第三方组件许可见 `THIRD_PARTY-NOTICES.md` 与各 `VENDOR-RECORD.txt`。
