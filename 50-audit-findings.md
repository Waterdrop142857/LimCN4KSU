# 代码清查结果

版本：2026-11-24。范围：`ksu-native-design` 的内容管线、原生运行时、WebUI、后台协议及模块安装链路。静态清查、主机测试、arm64 全量编译与模块打包均已执行；**没有 Android 实机验证**。原始仓库 `D:\BYBUS\source` 保持独立。

本文件只记录“当前代码是什么状态、哪些已补齐、哪些还没做”，不记录过程。

## 1. 本轮已补齐

| 能力 | 位置 | 说明 |
| --- | --- | --- |
| IL2CPP 绑定对齐 Unity 6 语义 | `src/runtime/il2cpp.h`/`.cpp` | GCHandle 改为 `void *`（不再截断成表索引）；新增 `class_get_fields`、`field_get_name/type/flags/value/set_value/static_get_value`、`class_get_name/namespace/is_enum/from_type`、`class_get_type/type_get_object`、`method_get_class/is_generic/get_flags` |
| 严格方法签名校验 | `src/runtime/il2cpp.cpp` | 新增 `find_verified_method`：声明类、参数类型、返回类型、泛型状态与静态性全部匹配才返回，未验证签名不会被 Hook |
| 翻译索引编译 | `src/translation/index_compiler.cpp`/`.h` | 移植 `TranslationIndexCompiler.kt`：unique/terms/contextual 三类索引、冲突消解、术语过滤与上下文键规则，写 schema 9（`LZTI1\0`）二进制 |
| 索引读取与文本路由 | `src/runtime/translation_index.cpp`、`src/runtime/text_router.cpp` | 索引在装 Hook 前解析，查表不碰磁盘；路由顺序为 JSON 表 → UI 状态标签 → contextual → unique → terms → 原文 |
| 清单 `index` 与三描述符交付 | `src/translation/snapshot_builder.cpp`、`src/translation/snapshot.cpp`、`src/runtime/companion.cpp`、`src/runtime/snapshot_reader.cpp` | 清单新增 `schema/path/sha256/bytes/unique/terms/contextual`；builder 写出 `index/index.bin`；companion Hello 交付 manifest/font/index 三个 fd；reader 校验字节数、SHA-256 与三个计数；无 index 的旧 generation 兼容 |
| 文本 Hook 扩展 | `src/runtime/hooks.cpp` | 在 `LoadLocalizeFileSync`、`TextAsset.get_text`、UGUI `set_text` 之外新增 TMP `set_text`/`set_text_string`、`Announcer.SetDescription`、`StorySystem.Dialog..ctor`、`RPGSystem.RpgLocText_Dialogue.GetTextData`、`ChoiceScrollViewBox.SetText`、`NoticeUIContentText.SetData`、`ActionChoiceButton.SetButtonData`，以及通用对象字符串字段翻译 |
| AppSealing 计划增字段 | `src/translation/appsealing_profile.cpp`、`src/translation/snapshot.cpp`、`src/runtime/appsealing.cpp` | `profile.appsealing` 增加 `guards`、`exactProfile`、`reportStable`；运行时在装任何补丁前逐字复核 `guards` |
| APK 资格解析 | `src/service/apk_signature.cpp`、`src/service/android_manifest.cpp`、`src/service/native_alignment.cpp` | APK Signing Block v2/v3 证书 SHA-256、AXML `<application android:name>`、PT_LOAD 对齐/必需库/重复库；`game_scan` 读取 SDK、页大小、ABI、`lastUpdateTime`、签名与 Application |
| 构建资格门 | `src/translation/snapshot_builder.cpp` | `BuildInputs` 增加资格字段；SDK < 26、非 arm64、对齐失败、非官方签名返回 `unsupported_runtime`，不发布快照 |
| WebUI 状态位 | `webui/app.js` | 概览显示 `game.profileState`/`profileCode`，诊断显示 `runtime.compatState`；新增 `index_invalid`、`snapshot_index_missing`、`snapshot_index_invalid`、`appsealing_guard_mismatch`、`font_component_rejected` 文案 |

## 2. 先前问题与修复状态

| 优先级 | 位置 | 影响与证据 |
| --- | --- | --- |
| 高 | `src/shared/json.cpp:13,74` | 单个 JSON 文档超过 64 MiB 被 `json_too_large` 拒绝。汉化包导入会整体失败；构建时大表会被跳过。70+ MiB 总文本若分散于多个小文件，不会仅因总量触发。 |
| 高 | `src/translation/snapshot.cpp:225` | `mergedBytes > 64 MiB` 的单表被判无效；即使解除 JSON 解析上限，快照仍无法发布或加载。 |
| 高 | `src/runtime/companion.cpp:173,350` | 上一次 Hook 已安装但未满 15 秒确认的 receipt 会使整个 generation 永久降级。正常在 15 秒内退出，或并发启动第二个游戏实例，也可能触发误判。当前没有区分正常退出与崩溃的证据。 |
| 高 | `module/uninstall.sh:15` | 卸载脚本仅检查 PID 为数字便 `kill`。守护进程异常退出留下旧 PID、该 PID 被复用时，可能误杀无关进程；`src/service/bybusd_main.cpp:101` 只在正常退出时删除 PID 文件。 |
| 中 | `src/service/job_queue.cpp:248` | 恢复逻辑跳过终态任务，导致守护进程重启后 WebUI 概览不显示最近完成的任务。`jobs/` 超过 512 个文件时，`list_dir` 返回失败，未完成任务恢复也整体跳过。 |
| 中 | `src/service/job_queue.cpp:94` | 去重键遗漏 `JobSpec.activate`。相同归档同时以相反激活选项提交时，后一个请求复用前一个任务，激活意图丢失。 |
| 中 | `src/service/ipc_server.cpp:83` | `stop()` 与 `serve()` 跨线程无同步地读写 `socket_` 并关闭描述符，构成 C++ 数据竞争。 |
| 低 | `src/translation/snapshot.cpp:98` | `manifest.json` 仍有 32 MiB 固定上限。它是索引元数据上限，不是汉化正文总量上限；极多表项可能触发。 |
| 低 | `webui/app.js:1014` | PAT 请求失败提示仍让用户检查仓库读取权限，与公开 Release 无需额外仓库权限的界面说明矛盾。 |

上述各项的处置：

- **内容管线**：解除 JSON 输入、单表及 manifest 的旧固定大小上限；游戏原文表读取或目录枚举失败明确报错，避免静默发布缺表快照；技能上下文不再在 256 字符处截断。`tests/host_smoke.cpp` 覆盖超过旧阈值的内容与长技能 ID。
- **后台与安装**：任务恢复扫描超过 512 个文件并保留最近终态；历史任务查询不污染最新任务指针；去重键加入 `activate`；IPC 停止标记和 socket 生命周期消除数据竞争；卸载只通过私有控制 socket 请求守护进程退出，不再对旧 PID 直接 `kill`。
- **运行时**：短时正常退出和并发启动不再自动产生永久降级标记；字体 SHA-256 在安装 Hook 前由监控线程验证；非关键 receipt 写入移出 Unity 回调；汉字判断加入 UTF-16 代理对处理。`webui/app.js` 已更正 PAT 权限提示，并在页面说明同源存储限制。
- **AppSealing 注入**：新增 `src/runtime/appsealing.cpp`/`.h`、`src/runtime/owned_code.h`、`src/translation/appsealing_profile.cpp`/`.h`。`profile.appsealing` 由 `bybusd` 从本机 APK 结构资格生成（`libcovault-appsec.so` 与 `libil2cpp.so`），运行时按计划安装 JNI detach 兼容点、50048 报告桥、70034/50040 站点补丁、471 瞬时 50048 维护与 SIGILL 守卫；补丁账本同时覆盖两个库；任一步失败即回滚并回执 `unsupported_runtime`。`profileState` 增加 `unsupported`，receipt 与 `diagnostics.runtime` 增加 `compatState`（`unknown|none|installed|rejected`）。

## 3. 仍未完成

1. **移植头文件未接入**：`src/runtime/rich_text.h`、`font_proof.h`、`flow_text_proof.h`、`managed_state.h`、`font_materials.h`、`font_scale.h`、`text_provenance.h`、`method_flags.h` 为上游逐字节移植，已单独做 arm64 编译，但没有被任何 `.cpp` 引用，也不在 `build/build.ps1` 的编译单元列表中。TMP 材质、字号、多语言字体的 setter 与富文本/注音 Hook 因此都未安装。
2. **状态机未移植**：剧情、RPG、选项、通知只做通用字符串字段与参数翻译；原版的 layout/flow/choice 状态机（`flow_text_proof`、`text_provenance` 所依赖的组件级证明）没有接入。
3. **`android_dlopen_ext` watcher 未实现**：仍按 100 ms 轮询 `resolve_il2cpp`（上限 900 次）与类可用性（上限 600 次）。
4. **SIGSEGV 守卫未实现**：评估结论是它服务于 VirtualApp `MapsRedirector` 的误报防护，本模块没有该路径，因此不移植。
5. **应用级管理未实现**：coverage、source 备份、图标、更新链接等不在范围。
6. **AI/容器/自更新/计费/迁移未实现**：设计已排除。
7. **索引的字段使用范围**：`profile.appsealing.exactProfile` 会被序列化与解析校验，但 `bybusd` 不写入、运行时也不比对；`index_invalid` 与 `font_component_rejected` 只有 WebUI 文案，尚无产生点。
8. **无 arm64 实机记录**：L0–L3 四层全部未验证。

## 4. 待验证的风险

- `src/runtime/hooks.cpp:126`、`src/runtime/snapshot_reader.cpp:130`：首次命中大表时在回调线程同步 IPC、读盘并计算 SHA-256；若回调位于 Unity 主线程，可能产生卡顿。`try_enable_translation` 也会同步等待 companion。
- 新增的 8 个 Hook 只是“签名对得上就安装”：`StorySystem.Dialog..ctor` 与 `ActionChoiceButton.SetButtonData` 的参数语义、选项/通知的调用频率与重入行为都没有实机数据；任一 Hook 安装失败会整体回滚并让本次启动不汉化。
- 索引在真实字典量级下的内存占用（unique/terms/contextual 各上限 200000，文件上限 64 MiB）与加载耗时未测。
- 构建期资格检查读的是**当前**安装包：游戏更新后签名、对齐或 ABI 变化会直接让构建失败（`qualification_*`），需要按版本重新确认而不是复用旧结论。
- 配对深度 64 层仍沿用原实现；需要真实汉化包与游戏原文做差分验证。该规则不限制 70+ MiB 总正文。
- 已保存的 PAT 位于 KernelSU 管理器 WebView `localStorage`。当前 KernelSU 管理器的模块 WebUI 共享 `mui.kernelsu.org` 来源；其他模块页面理论上可读取此键。应只使用专用、无额外权限的 PAT。安装的模块本身拥有 root 桥接，此存储并不提供跨模块隔离。

## 5. 验证边界

- 当前构建与 host smoke 测试通过；这只能证明编译、打包和纯数据层样例，不覆盖上述边界情况。host smoke 覆盖索引编译/写入/读取往返、含 `guards` 的清单往返与含 `index` 的清单校验。
- `40-validation-matrix.md` 的 L0–L3 仍全为“未验证”。Zygisk Hook、AppSealing 反馈、语言资源是否重下、字体路径和 WebUI 设备行为均不构成通过依据。
- 本轮的产物已重新生成 ZIP，但尚无设备：WebUI、Hook、字体、索引查表、游戏保护和资源下载行为均未验证。AppSealing 注入（报告桥、70034/50040/50048 站点、471 瞬时维护、SIGILL 守卫、双库账本、`guards` 复核）与构建期资格门都只经过桌面构建与静态检查，没有任何 arm64 实机记录，保护检查的实际时机与反应仍需设备观察。
