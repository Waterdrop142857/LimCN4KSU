# 代码清查结果

版本：2026-11-24。范围：`ksu-native-design` 的内容管线、原生运行时、WebUI、后台协议及模块安装链路。静态清查、主机测试、arm64 全量编译与模块打包均已执行；**没有 Android 实机验证**。参考实现仓库保持独立。

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
| WebUI 状态位 | `webui/app.js` | 概览显示 `game.profileState`/`profileCode`，诊断显示 `runtime.compatState`；新增 `index_invalid`、`snapshot_index_missing`、`snapshot_index_invalid`、`appsealing_guard_mismatch`、`appsealing_exact_mismatch`、`font_component_rejected` 文案 |
| TMP 材质与富文本接入 | `src/runtime/hooks.cpp` | 按组件保存原字体/共享材质/布局；中文回退字体、`GetFallbackMaterial`、`font_scale` ratio、材质 setter、`WriteMaterialProperty`、`AddMaterialReference`、六类富文本入口与三个 Ruby formatter |
| 加载事件与 exactProfile | `src/runtime/library_watch.cpp`、`src/translation/appsealing_profile.cpp`、`src/runtime/appsealing.cpp` | `android_dlopen_ext` 等待 `libil2cpp.so`；计划生成 SHA-256 exactProfile，解析与运行时两次重算比对 |
| 文本质量门禁 | `src/translation/text_policy.h`/`.cpp` | 新增 `is_blank`（Kotlin `String.isBlank()` 语义）与 `is_usable_display_text`（拒 U+FFFD、除 `\n\r\t` 外的 C0、`0x7F..0x9F`）。调用点：`json_pairer.cpp` 的显示叶子、display-array 逐项、`released_texts` 两处、`count_fields` 两处，`index_compiler.cpp` 的 `add()` |
| 配对数值绑定 | `src/translation/json_pairer.cpp` | `matching_structure_impl` 由 `as_double()` 改为整数 int64 精确比较、其余按十进制归一化比较；新增 1000000 节点遍历上限（对齐 `TranslationSnapshot.kt:36`） |
| ID 边界 | `src/translation/json_pairer.cpp`、`src/translation/index_compiler.cpp` | `is_skill_number` 限 1..18 位；作用域标识与 story scene 名限 1..96 字符；`@story:` 上下文严格整串匹配 `^@story:[A-Za-z0-9_-]{1,96}(/dialog:[0-9]{1,18})?$` 且保序去重 |
| 配对覆盖率门禁 | `src/translation/json_pairer.h`、`src/translation/snapshot_builder.cpp`、`src/service/job_queue.cpp` | 自由函数 `sufficiently_paired()`；通过 90% 门槛的表数写入 `available_source_file_count`；不足时不写 `index/index.bin`、`manifest.index.present=false`、`BuildOutcome.detail="insufficient_pairs"`，快照仍发布；成功路径的 `detail` 也可被 `job status` 读到 |
| 索引参考布局 | `src/translation/index_compiler.cpp`/`.h`、`src/runtime/translation_index.cpp` | `LZTI1\0` + schema 9 + 三个计数，随后三组「`u32` 长度 + UTF-8」条目，按插入序写出，无尾部/填充/校验和；读取端同步 |
| 索引摘要与去重键 | `src/translation/index_compiler.cpp` | `inputSha256` 输入序列为 `path, context, field, source, translation, stable_id`；排序与去重键补 `context`，同 path/field/文本但上下文不同的记录不再合并 |
| 装钩失败语义 | `src/runtime/hooks.cpp`、`src/runtime/hooks.h` | 只有两个入口钩子（`LoadLocalizeFileSync`、`TextAsset.get_text`）失败才回滚并回执 `unsupported_runtime`；其余钩子失败只记日志；成功位为 `LoadLocalizeFileSync && TextAsset.get_text && TMP_Text.set_text` 三个 trampoline |
| 方法签名归一化 | `src/runtime/type_signature.h` | 新增 header-only 归一化（去空格、`/`→`.`、去泛型 arity，只按 `<>` 深度 0 的逗号切分）；`AddMaterialReference` 由「泛型逗号切错、Hook 永不安装」恢复为可安装；三个 ruby 类同地址共享 trampoline |
| 字体尝试预算 | `src/runtime/hooks.cpp` | 一次性闩锁改为最多 3 次（对齐参考 `g_font_attempts >= 3`） |
| 内联空间校验 | `src/runtime/hooks.cpp` | 新增方法地址表与 `has_inline_hook_space`；未命中只记日志并继续安装（fail-open，见 §4） |
| 完整性口径 | `src/runtime/owned_code.h`、`src/runtime/integrity.cpp`/`.h`、`src/translation/snapshot.cpp` | 区间上限 512→256、允许零长区间与零长 section；新增 `verify_owned_sections()` 两段共用累加器且 `owned == substitute_count`；审计失败不阻断装钩，只写一行 `hook audit ...` 日志；`profile.appsealing.rangesCount` 构建期上限同步为 256 |
| 快照表缓存 | `src/runtime/cache_key.h`、`src/runtime/snapshot_reader.cpp` | 缓存键改为 `(generation, digest, bytes, 实际长度)`；`SnapshotReader::open()` 首行清空缓存；`prewarm_tables()` 走同一条「摘要校验后才入缓存」路径 |
| 描述符生命周期 | `src/runtime/companion.cpp` | degraded 分支补关 `index_fd`；超限 SCM_RIGHTS 描述符逐个关闭；「收到数量 ≠ 头声明」即失败 |
| 服务层边界 | `src/service/android_manifest.cpp`、`src/service/apk_signature.cpp`、`src/service/native_alignment.cpp` | AXML 长度前缀先判边界再读取并限 4 字节续延；APK Signing Block `block_size < 24` 直接拒绝；PT_LOAD 要求 `memsz != 0 且 memsz <= file_size`、`vaddr <= file_size - memsz`，重叠判定不会 u64 回绕 |
| 主机测试基建 | `tests/harness.h`/`.cpp`、`tests/test_main.cpp`、`build/test-host.ps1` | 新增共享 harness 与用例文件（`pairing_fidelity`、`index_format`、`type_signature`、`integrity_parity`、`cache_keys`、`bounds`）；`build/test-host.ps1` 编译 `tests/*.cpp` 全部 |

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
| 低 | `src/translation/snapshot.cpp:98` | `manifest.json` 曾有 32 MiB 固定上限，极多表项可能触发。已修复：该上限已解除（见下方「内容管线」）。 |
| 低 | `webui/app.js:1014` | PAT 请求失败提示仍让用户检查仓库读取权限，与公开 Release 无需额外仓库权限的界面说明矛盾。 |

上述各项的处置：

- **内容管线**：解除 JSON 输入、单表及 manifest 的旧固定大小上限（`src/shared/json.cpp`、`src/translation/snapshot.cpp`）；游戏原文表读取或目录枚举失败明确报错，避免静默发布缺表快照；技能上下文改按 `is_skill_number` 的 1..18 位数字边界判定，超界 id 不再生成 `@skill:` 上下文（`src/translation/json_pairer.cpp`）。`tests/host_smoke.cpp` 覆盖超过旧阈值的内容与非法技能 id。
- **配对与索引（已修复：按参考实现对位，桌面编译与主机测试通过，实机未验证）**：空白、含 U+FFFD、含控制符的译文不再进入索引，也不会用空串覆盖原文（`src/translation/text_policy.h`/`.cpp` 的 `is_blank`/`is_usable_display_text`，调用点在 `json_pairer.cpp` 与 `index_compiler.cpp` 的 `add()`）；索引二进制改为参考布局（`src/translation/index_compiler.cpp`，读取端 `src/runtime/translation_index.cpp`）；`inputSha256` 输入序列与去重键补 `context`；配对覆盖率不足 90% 时不再激活索引（`src/translation/snapshot_builder.cpp`、`src/service/job_queue.cpp`）。`tests/pairing_fidelity.cpp`、`tests/index_format.cpp` 覆盖上述规则。
- **装钩回滚（已修复：按参考实现对位，桌面编译与主机测试通过，实机未验证）**：不再「任一钩子失败即整机回滚」。只有 `LoadLocalizeFileSync` 与 `TextAsset.get_text` 两个入口钩子失败才回滚并回执 `unsupported_runtime`，其余钩子（UGUI `set_text`、TMP `set_text`/`set_text_string`/`set_font`/材质、`Announcer.SetDescription`、剧情/RPG/选项/通知、富文本、ruby）失败只记日志；成功位为三个 trampoline 同时就绪（`src/runtime/hooks.cpp`、`src/runtime/hooks.h`）。
- **完整性口径（已修复：按参考实现对位，桌面编译与主机测试通过，实机未验证）**：`src/runtime/owned_code.h` 区间上限收紧到 256 并允许零长区间/零长 section；`src/runtime/integrity.cpp` 的 `verify_owned_sections()` 两段共用累加器且 `owned == substitute_count`；审计失败不再阻断装钩，只写一行 `hook audit ...` 日志。`tests/integrity_parity.cpp` 覆盖区间预算与零长条目。
- **后台与安装（已修复：`src/service/job_queue.cpp`、`src/service/ipc_server.cpp`、`module/uninstall.sh`）**：任务恢复扫描超过 512 个文件并保留最近终态；历史任务查询不污染最新任务指针；去重键加入 `activate`；IPC 停止标记和 socket 生命周期消除数据竞争；卸载只通过私有控制 socket 请求守护进程退出，不再对旧 PID 直接 `kill`。
- **运行时（已修复：`src/runtime/companion.cpp`、`src/runtime/state.cpp`、`src/shared/utf.cpp`）**：短时正常退出和并发启动不再自动产生永久降级标记；字体 SHA-256 在安装 Hook 前由监控线程验证；非关键 receipt 写入移出 Unity 回调；汉字判断加入 UTF-16 代理对处理。`webui/app.js` 已更正 PAT 权限提示，并在页面说明同源存储限制。
- **AppSealing 注入**：新增 `src/runtime/appsealing.cpp`/`.h`、`src/runtime/owned_code.h`、`src/translation/appsealing_profile.cpp`/`.h`。`profile.appsealing` 由 `bybusd` 从设备上已安装 APK 的结构资格生成（`libcovault-appsec.so` 与 `libil2cpp.so`），运行时按计划安装 JNI detach 兼容点、50048 报告桥、70034/50040 站点补丁、471 瞬时 50048 维护与 SIGILL 守卫；补丁账本同时覆盖两个库；任一步失败即回滚并回执 `unsupported_runtime`。`profileState` 增加 `unsupported`，receipt 与 `diagnostics.runtime` 增加 `compatState`（`unknown|none|installed|rejected`）。

## 3. 仍未完成

1. **TMP 字体/材质/富文本已接入**：`hooks.cpp` 保存组件原字体/材质/布局，切换中文回退字体，按 `TMP_MaterialManager.GetFallbackMaterial` 转换共享材质，并用 `font_scale` 保持 shader ratio；`set_font`/`set_sharedMaterial`/`set_material`、`TextMeshProMaterialSetter.WriteMaterialProperty`、`MaterialReference.AddMaterialReference`、`TMProStringMatcher` 六类格式入口与三个 Ruby formatter 已接 Hook。剧情场景上下文 `StoryData.GetScenario`、选项结果列表 `ChoiceScrollViewBox.SetTotalContent` 与 RPG 对话 `@rpg-dialogue:<key>/text:<index>` 上下文也已接入。未验证：真实构建的签名命中率、材质引用计数、Ruby 调用频率与 UI 回收行为。
2. **状态机仅部分移植**：剧情、RPG、选项、通知按 `text_policy` 显示字段白名单翻译，并保存/恢复 TMP wrapping/overflow；原版 layout/flow/choice 的完整组件级证明状态机仍未逐项移植。
3. **`android_dlopen_ext` watcher 已实现**：`src/runtime/library_watch.cpp` Hook 加载事件等待 `libil2cpp.so`，并在等待期间分片重新探测；Hook 不可用或加载路径不同才回退到有界轮询。
4. **SIGSEGV 守卫已移植**：`src/runtime/sigsegv_guard.cpp` 安装 SIGSEGV 包装器，包含 471 回溯器恢复分支、旧处理器链式调用、上下文变化检测与 AppSealing 覆盖后的重包裹；`profile.appsealing.sigsegv` 由 471 结构家族生成。尚无 arm64 实机验证。
5. **应用级管理明确排除**：coverage、source 备份、图标、更新链接不在本模块范围，未实现。
6. **AI/容器/自更新/计费/迁移未实现**：设计已排除。
7. **exactProfile 已闭环**：`bybusd` 在计划完成后按结构字段生成 SHA-256，清单解析与运行时安装补丁前各重算一次；缺失、空串或字段不一致都会被拒绝。`index_invalid` 与 `font_component_rejected` 已有运行时产生点，但只经过静态/主机验证。
8. **无 arm64 实机记录**：L0–L3 四层全部未验证。
9. **辅助判定仍是零引用符号**：`src/runtime/font_proof.h` 的 `Catalogue`，以及 `src/runtime/text_provenance.h` 中的作用域键、可见长度、注音与格式化参数辅助函数（`skill_number`、`gift_*`、`event_*`、`guide_*`、`single_line_label`、`positioned_ruby`、`is_short_translated_label`、`contains_translated_field` 等）没有调用点；`src/runtime/font_scale.h` 的 `ratios_off` 从未被置位（对应参考实现的 `RATIOS_OFF`）。换行恢复与人格标题重排未移植。

## 4. 待验证的风险

- `src/runtime/hooks.cpp`、`src/runtime/snapshot_reader.cpp`：首次命中大表时在回调线程同步 IPC、读盘并计算 SHA-256；若回调位于 Unity 主线程，可能产生卡顿。`try_enable_translation` 也会同步等待 companion。
- 新增 Hook 只是“签名对得上就安装”：`StorySystem.Dialog..ctor`、`StoryData.GetScenario` 与 `ActionChoiceButton.SetButtonData` 的参数语义、选项/通知的调用频率与重入行为都没有实机数据；单个 Hook 失败只记日志并让该类文本回退原文，只有两个入口钩子失败才整次回滚。
- 字体切换优先使用 `translate_managed_string` 的 recent-output provenance + `FlowProof` 前缀证明，并对“含汉字且不含假名”的表级合并文本保留保守 fallback；未翻译的纯汉字日文仍可能被换字体，含假名的未翻译文本不会被换。仍需实机确认 typewriter 重排、Ruby 输出和缓存文本。
- `String[]` 翻译依赖 `il2cpp_array_get`/`il2cpp_array_set` 导出；若这两个在目标 IL2CPP 中只是宏，数组字段会保持原文，日志会给出提示。`List<string>` 只在 `successDesc`/`failureDesc`/`result` 这类数组字段上原地写回，最多 64 项。
- 索引在真实字典量级下的内存占用（unique/terms/contextual 各上限 200000，文件上限 64 MiB）与加载耗时未测。
- 构建期资格检查读的是**当前**安装包：游戏更新后签名、对齐或 ABI 变化会直接让构建失败（`qualification_*`），需要按版本重新确认而不是复用旧结论。
- 配对深度 64 层仍沿用原实现；需要真实汉化包与游戏原文做差分验证。该规则不限制 70+ MiB 总正文。
- 已保存的 PAT 位于 KernelSU 管理器 WebView `localStorage`。当前 KernelSU 管理器的模块 WebUI 共享 `mui.kernelsu.org` 来源；其他模块页面理论上可读取此键。应只使用专用、无额外权限的 PAT。安装的模块本身拥有 root 桥接，此存储并不提供跨模块隔离。
- **prewarm 预算代价**：`prewarm_tables()` 与按需读取共用 32 MB / 32 条 LRU（`src/runtime/snapshot_reader.cpp`）。表多或总量大时，前面预热的表会被淘汰，首次命中仍可能回退到同步 IPC + 读盘 + 哈希。
- **`has_inline_hook_space` fail-open**：地址表未命中时只记日志并继续安装（`src/runtime/hooks.cpp`）。采集范围尚未实机证明，判错的代价是一次越界补丁而不是跳过。
- **`rangesVaddr` 校验口径**：`src/translation/snapshot.cpp` 仍以 512 MiB（`kMaxModuleBytes`）作为 `rangesVaddr` 与描述符窗口上限，而不是清单里的 `moduleSize`；超大模块的越界描述符只能由运行期 `ranges_crc` 拦截。
- **`rangesFingerprint` 未比对**：构建期只校验其十六进制格式，不与运行期重算的 `owned_code::range_fingerprint()` 比较。
- **主机构建替身**：`tests/bounds.cpp` 内含 `translation::read_archive` 的主机替身；把 `src/translation/archive_reader.cpp` 加入主机构建时必须先删除该替身，否则会出现两个定义。
- **WebUI 不展示 `job.detail`**：`webui/app.js` 没有读取 `detail` 字段，因此 `insufficient_pairs`、`qualification_*`、`build_index_*` 等构建细分理由只能用 `bybusctl job status` 读取。

## 5. 验证边界

- 当前构建与主机测试通过；这只能证明编译、打包和纯数据层样例，不覆盖上述边界情况。`build/test-host.ps1` 编译 `tests/*.cpp` 全部：`host_smoke` 覆盖索引编译/写入/读取往返、含 `guards` 的清单往返与含 `index` 的清单校验、AppSealing exactProfile 拒绝路径、富文本/FlowProof、布局 override、字体比例计算与 provenance 有界淘汰；`pairing_fidelity`、`index_format`、`type_signature`、`integrity_parity`、`cache_keys`、`bounds` 覆盖本轮对位的门禁、布局、归一化、区间预算、缓存键与解析边界。
- `40-validation-matrix.md` 的 L0–L3 仍全为“未验证”。Zygisk Hook、AppSealing 反馈、语言资源是否重下、字体路径和 WebUI 设备行为均不构成通过依据。
- 本轮的产物已重新生成 ZIP，但尚无设备：WebUI、Hook、字体、索引查表、游戏保护和资源下载行为均未验证。AppSealing 注入（报告桥、70034/50040/50048 站点、471 瞬时维护、SIGILL 守卫、双库账本、`guards` 复核）与构建期资格门都只经过桌面构建与静态检查，没有任何 arm64 实机记录，保护检查的实际时机与反应仍需设备观察。
