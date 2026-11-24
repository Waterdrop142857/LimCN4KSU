# BYBUS → KernelSU 原生模块移植实施方案

版本：2026-11-24。基准：参考实现仓库（独立目录，不并入本仓库）提交 `bd32608`。规范来源：`01-implementation-design.md`、`02-code-quality-standard.md`。本目录独立于参考实现仓库，产物不写入 `ksu-native-design/` 之外。

## 1. 运行边界与目录映射

| 设计规范板块 | 本实现目录 | 说明 |
| --- | --- | --- |
| `webui` | `webui/` 源 → `module/webroot/` 构建产物 | Vite + 官方 `kernelsu` 库；页面只调用 `kernelsu.exec` |
| `control` | `src/control/` | `bybusctl`：参数校验、IPC 客户端、固定 JSON 响应 |
| `service` | `src/service/` | `bybusd`：任务队列、代次发布、原文发现、字体准备 |
| `translation` | `src/translation/` | 纯数据：归档导入、配对器、快照构建 |
| `runtime` | `src/runtime/` | Zygisk 模块、IL2CPP 发现与 Hook、快照/索引查找与文本路由、字体 |
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
    index/index.bin          # schema 9 翻译索引（本轮新增）
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
| `container/TranslationJsonPairer.kt` | `src/translation/json_pairer.cpp` | 逐字段等价：稳定 ID、重复 ID 消歧、嵌套数组、技能/罪恶共鸣/事件/指南/RPG 对话作用域、贪心剩余配对；技能 id 限 1..18 位、作用域标识与 story scene 名限 1..96 字符 |
| `container/TranslationTextPolicy.kt` | `src/translation/text_policy.cpp` | 显示字段白名单与 `STRING_ARRAY_FIELDS`；`is_blank`/`is_usable_display_text` 对齐参考的空白与编码损坏判定（空串、U+FFFD、除 `\n\r\t` 外的 C0、`0x7F..0x9F`），不做翻译内容质量评分 |
| `container/TranslationSnapshot.kt` | `src/translation/json_pairer.cpp`（快照绑定部分） | 标记 `_bybusTextSnapshot`、编码/解码、`matchingStructure`（整数 int64 精确比较、其余十进制归一化，节点上限 1000000） |
| `translation/LocalTranslationArchive.kt` | `src/translation/archive_import.cpp` | ZIP/7z 签名识别、安全路径、重复目标、`mapResource` 映射；不移植数量与内容大小上限 |
| `update/TranslationUpdateManager.kt` | `src/service/source_fetch.cpp` | 固定仓库 `LocalizeLimbusCompany/LocalizeLimbusCompany` 的最新 Release，资产名 `LimbusLocalize_<数字>.7z` |
| `third_party/.../LimbusTranslationRuntime.cpp` | `src/runtime/` | 只保留 IL2CPP 发现、`TextAssetPatchManager.LoadLocalizeFileSync` / `TextAsset.get_text` 读后合并、字体准备；移除 VirtualCore/JNI 回调/容器重定向 |
| `.../Foundation/LimbusCodeIntegrityCompat.cpp`（报告桥与站点补丁） | `src/runtime/appsealing.cpp`、`src/runtime/appsealing.h` | 移植 JNI detach 兼容点、50048 报告桥、70034/50040 站点补丁、471 瞬时 50048 维护与 SIGILL 守卫；偏移、期望指令字与栈布局全部来自设备上已安装 APK 的清单，不写死版本常量 |
| `.../Foundation/LimbusOwnedCode.h` | `src/runtime/owned_code.h` | 保留自有代码段的范围表示，只用于按清单重算范围 CRC 与指纹；账本同时覆盖 `libil2cpp.so` 与 `libcovault-appsec.so` |
| `.../Foundation/IORelocator64.cpp`（AppSealing 部分） | `src/translation/appsealing_profile.cpp`、`src/translation/appsealing_profile.h` | 只移植站点资格判定与计划生成（`profile.appsealing`）；容器 I/O 重定向、`MapsRedirector` 与 `syscall` Hook 不移植 |
| `And64InlineHook.cpp`、`SubstrateHook.cpp` | `third_party/substrate/` | 原文件与 MIT/BSD 声明一并移植 |
| `container/TranslationIndexCompiler.kt` | `src/translation/index_compiler.cpp`/`.h`、`src/runtime/translation_index.cpp`/`.h` | 逐规则等价：unique/terms/contextual 三类映射、显示与术语过滤、冲突消解、确定性摘要；二进制写 `LZTI1\0` + schema 9，读取端在校验魔数、schema 与三个计数后建表 |
| `Foundation/LimbusRichText.h` | `src/runtime/rich_text.h` | 逐字节移植；只提供富文本判定，不翻译、不改写标签 |
| `Foundation/LimbusFontProof.h`、`LimbusFlowTextProof.h` | `src/runtime/font_proof.h`、`flow_text_proof.h` | 逐字节移植；组件级“完整文本已证明”判定与渐进视图 |
| `Foundation/LimbusFontMaterials.h`、`LimbusFontScale.h` | `src/runtime/managed_state.h`、`font_materials.h`、`font_scale.h` | 逐字节移植；托管对象租约、材质回填与字号基准，已接入 TMP 字体/共享材质/shader ratio |
| `Foundation/LimbusTextProvenance.h` | `src/runtime/text_provenance.h` | 逐字节移植；只拥有本模块写过的值，游戏后续写入优先 |
| `Foundation/LimbusUiLabels.h`、`LimbusMethodFlags.h` | `src/runtime/ui_labels.h`、`method_flags.h` | `ui_labels` 已接入 `text_router`；`method_flags` 已用于 `StoryData.GetScenario` 的实例方法判定 |
| `helper/compat/LimbusNativeProfile.java`、`LimbusNativeStructure.java` | `src/service/apk_signature.cpp`、`src/service/android_manifest.cpp`、`src/service/native_alignment.cpp` | 用原生实现替掉 Java 侧 `PackageManager` 查询：APK Signing Block v2/v3 证书 SHA-256、AXML `<application android:name>`、PT_LOAD 对齐/必需库/重复库；资格判定语义与 `requireLibraryAlignment` 一致 |

### 2.1 运行时覆盖现状

| 环节 | 已接入 | 未接入 |
| --- | --- | --- |
| IL2CPP 绑定 | GCHandle `void *`、字段反射、`find_verified_method` | — |
| Hook | `LoadLocalizeFileSync`、`TextAsset.get_text`、UGUI `Text.set_text`、TMP `set_text`/`set_text_string`/`set_font`/`set_sharedMaterial`/`set_material`、`Announcer.SetDescription`、`StorySystem.Dialog..ctor`、`StoryData.GetScenario`、`RPGSystem.RpgLocFormat.Apply`、`RPGSystem.RpgLocText_Dialogue.GetTextData`、`ChoiceScrollViewBox.SetText`/`SetTotalContent`/`ReadingContent`/`Clear`/`OnDisable`、`NoticeUIContentText.SetData`、`ActionChoiceButton.SetButtonData`、`TextMeshProMaterialSetter.WriteMaterialProperty`、`MaterialReference.AddMaterialReference`、`TMProStringMatcher` 六类格式入口、三个 Ruby formatter、按显示字段白名单翻译的通用对象字符串字段 | 原版剧情/RPG/选项/通知的完整 layout/flow/choice 组件级证明状态机 |
| 装钩失败语义 | 只有两个入口钩子（`LoadLocalizeFileSync`、`TextAsset.get_text`）失败才回滚并回执 `unsupported_runtime`；其余钩子失败只记日志；成功位为 `LoadLocalizeFileSync && TextAsset.get_text && TMP_Text.set_text` 三个 trampoline | — |
| 方法签名匹配 | `src/runtime/type_signature.h` 归一化（去空格、`/`→`.`、去泛型 arity，按 `<>` 深度 0 切分）；三个 ruby 类同地址共享 trampoline | — |
| 文本解析 | JSON 表、UI 状态标签、contextual/unique/terms 索引（参考二进制布局，摘要与去重键含 `context`）、TMP wrapping/overflow 保存恢复 | 原版 layout/flow/choice 的完整状态机 |
| 交付与校验 | manifest/font/index 三描述符、索引字节数与计数校验、旧 generation 无索引兼容、配对覆盖率 90% 门槛（不足时索引不激活）、表缓存键 `(generation, digest, bytes, 实际长度)` | — |
| AppSealing | 计划 + `guards` 复核、`exactProfile` 重算比对、detach 兼容点、报告桥、站点补丁、瞬时维护、SIGILL 守卫、SIGSEGV 守卫、双库账本；自有代码段审计只记日志，不阻断装钩 | 实机检查时机与反馈 |
| 资格 | 构建期 SDK/ABI/PT_LOAD 对齐/官方签名四项门禁 | — |

字体、富文本与 provenance 头文件已由 `hooks.cpp` 引入并参与 arm64 编译；TMP 字体/材质/shader ratio、富文本六类入口、三个 Ruby formatter 与 `StoryData.GetScenario` 场景上下文已接入，`method_flags` 用于该场景方法的实例判定。尚未接入的是原版剧情/RPG/选项/通知的完整 layout/flow/choice 组件级证明状态机。Hook 先做签名验证再安装；失败只影响该 Hook 本身，只有两个入口钩子失败才回滚并回执 `unsupported_runtime`。实机是否全部命中仍需设备记录。

## 3. 与设计文档的已知偏差

1. **字体资产格式**：设计列 `assets/ChineseFont.ttf.xz`。本构建环境无 xz 编码器，模块 ZIP 本身已使用 deflate，故以 `assets/ChineseFont.ttf` 原样分发（SIL OFL 1.1，来源 `LocalizeLimbusCompany/Fonts/ChineseFont.ttf`），`bybusd` 构建快照时校验固定 SHA-256，再复制进 generation。压缩方式变更不改变校验语义。
2. **在线下载**：`bybusd` 不内嵌 TLS 栈。下载阶段委派给设备上可用的结构化取回器（`/system/bin/curl`，缺失时回退 `wget`/`/vendor/bin/curl`），argv 全为固定字面量与已校验的 URL，不经 shell；都不可用时任务以 `network_unavailable` 结束，并提示改用 `inbox/` 投放或 `import` 导入。
3. **`sources select` 之外的源管理**：首版只提供 `zero` 默认源与本地导入源（`local-<sha256 前 16 位>`）；AI 生成、撤回、图标、更新链接不移植。
4. **安装入口**：使用 KernelSU 自带安装器；顶层 `customize.sh` 只设置可执行文件权限，不写 `/data/adb/bybus`，也不更改 WebUI 的权限与 SELinux 上下文。
5. **收据词表未扩**：`hook_setup_incomplete` 不新增错误码（`src/shared/status.h` 词表冻结）；入口钩子不全时用既有 `rejected` + `unsupported_runtime` 表达，细节只写在日志里。
6. **`inputSha256` 不写入清单**：索引摘要按参考算法计算，但不落盘到 `manifest.json`；清单改用 `index.sha256` 文件摘要绑定索引，强于参考实现。
7. **`merge()` 未强制参考 `pair()` 的 `Unrecognized empty resource` 校验**：0 显示字段的表会被计入「已配对」，参考侧因异常被排除。影响只在覆盖率分子。
8. **覆盖率口径**：按包内原文资源并集统计（`inputs.pack.size()` 为分母），不做参考实现的 per-locale 取最大值——本移植只处理单语言包。
9. **`releasedTexts` 目录的产物仍未被消费**：字体覆盖判定留待后续阶段。
10. **`has_inline_hook_space` fail-open**：地址表未命中时只记日志并继续安装（`src/runtime/hooks.cpp`），参考实现在此处跳过。理由见 `50-audit-findings.md` §4。

## 4. 关键契约

- 快照条目同时绑定：源 id、归档 SHA-256、游戏安装标识、规范资源名、原文 UTF-8 摘要、合并结果摘要与字节长度。运行时按 `(资源名, 原文摘要)` 命中，文件名单独命中不替换。
- 索引与表同级：`manifest.json` 的 `index` 声明 schema 9、路径（只允许 `index/index.bin`）、SHA-256、字节数与三个计数，任一项与文件不符即整份清单被拒；`index` 与 `profile` 互不依赖，也都不阻止快照发布。
- 索引二进制布局：`LZTI1\0` + schema 9（u32）+ `uniqueCount`/`termCount`/`contextualCount`（三个 u32），随后三组条目；每条为「u32 长度 + UTF-8」的 key 与 value，三个映射按插入序写出，无尾部、填充或校验和。摘要输入序列为 `path, context, field, source, translation, stable_id`（`context` 即参考的 `id`），排序与去重键同此六项。
- 配对覆盖率门槛：通过 90% 配对门槛的表数作为 `CompiledTranslationIndex::available_source_file_count` 传入索引编译（不写入清单）；覆盖率不足时不写 `index/index.bin`、`manifest.index.present=false`、构建 `detail=insufficient_pairs`，快照仍发布。
- 索引与 `profile` 都参与 generation 摘要，因此“只重建索引”或“只换计划”都会得到新 generation，旧 generation 仍可服务正在运行的进程。
- 一个 generation 发布后只读；`active` 指针以 `rename` 原子切换；中断只留临时目录，启动时清理 `imports/`、`generations/.tmp-*`。
- 游戏进程启动时固定一个 generation；后台发布的新 generation 只服务下一次启动。
- 游戏主线程不做网络、解包、全量配对；运行时只用启动期载入的索引与每表一次性摘要查找。
- 诊断只含阶段、计数、版本摘要与受限错误码；不写原文、译文、账号、设备标识。

## 5. 开发顺序

1. `shared` + `translation` 纯数据层与桌面测试夹具。**已完成**
2. `service`（任务、代次、原文发现、字体、APK 资格）与 `control`。**已完成**
3. WebUI 静态页面。**已完成**（含 `profileState`/`profileCode` 与 `compatState` 展示）
4. `runtime`：空 Zygisk 模块 → `LoadLocalizeFileSync` 读后合并 → 索引查表与组件级 Hook → 字体 → 单表中文显示。**部分完成**：Hook 与索引已接入，实机未验证
5. 打包、校验、安装/禁用/卸载回归。**桌面已通过，实机未验证**
6. 剩余：原版剧情/RPG/选项/通知的完整 layout/flow/choice 组件级证明状态机。TMP 材质/字号、富文本/注音、`StoryData.GetScenario` 场景上下文与 SIGSEGV 守卫已接入，均无实机验证。

## 6. 未在设备上验证的未知

- 游戏原文目录与表名清单（`Localize/jp|en|kr/...`）需按实际安装确认。
- 加载器是否改变 JSON 字节表示（BOM、换行、预处理）需实测；不一致时该表回退原文并写诊断。
- 设备上由 root 守护进程读取 `/sdcard/Download/...` 的实际访问权限；失败时改用 `inbox/` 或其他可读绝对路径。
- 注入后的游戏保护反馈需按版本分别记录：AppSealing 兼容注入（detach 兼容点、50048 报告桥、70034/50040 站点、471 瞬时维护、SIGILL 守卫、`guards` 复核）已移植，但实际检查时机与反应仍要实机确认，见 `12-kernelsu-appsealing.md` 与 `40-validation-matrix.md` 的 L0–L3。
- `/proc/self/fd/<n>` 作为 Unity 字体路径是否被接受；不接受时使用辅助目录副本。
- 索引的实机表现：schema 9 二进制读取、`index/index.bin` 在真实字典量级下的体积与加载耗时、第三个描述符在 Unity 加载压力下的稳定性。
- 构建期资格门是否覆盖所有官方分发形态：分包 APK 的签名与对齐、Game 更新后的 SDK/ABI 变化都会让构建直接失败，需按版本重新确认。
