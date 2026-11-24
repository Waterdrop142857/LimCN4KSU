# Handoff — BYBUS KernelSU 原生移植（ksu-native-design）

版本：2026-11-24。交接对象：下一位维护者 / 实施代理。
本文件只写「现状、契约、坑、下一步」，不记录过程。所有结论都能在仓库里找到落点。

---

## 0. 一页速览

| 项 | 状态 |
|---|---|
| 代码基线 | 分支 `main`，HEAD `7970382`（本轮 **未提交**，56 个文件待提交） |
| 参考实现 | 独立仓库，HEAD `bd32608`（Kotlin `container/*.kt` + 原生 `Foundation/*.cpp`），**唯一权威**；不并入本仓库 |
| 源码规模 | `src/` 100 文件 / 18.5k 行；`tests/` 9 文件 / 1.75k 行 |
| 桌面验证 | 主机测试全绿（警告即错误）、arm64 交叉编译 zero-warning（`-Wall -Wextra -Werror`）、模块 ZIP 17 条目摘要自洽 |
| 实机验证 | **零**。`40-validation-matrix.md` 的 L0–L3 全部未验证，任何「注入可启动 / 汉化显示」结论都还没有 |
| 主要残留工作 | Phase 3（组件级 layout/flow/choice 状态机、字体 provenance 接线）+ 两个构建/运行口径分裂点 + 服务层并发模型 |

一句话：**数据层与索引层已与参考实现逐规则对齐；运行时层是「签名验证后装钩 + 部分生效」，只有桌面证据。**

---

## 1. 仓库布局与权威来源

本仓库是独立目录（下称「仓库根」），与参考实现仓库**并列但不合并**，两者之间只做只读对照。

```
<仓库根>/
  src/  tests/  webui/  module/  third_party/  tools/  build/   # 见 README.md「结构」
  *.md                                                          # 01…50 设计/验收文档 + 本文件
<参考实现仓库>/                                                 # 只读，勿改
  app/src/main/java/com/example/limbuszhcn/container/*.kt
  third_party/virtualapp-upstream/lib/src/main/jni/Foundation/*.cpp
```

- **构建入口只有 `build/` 下的一套 Windows 脚本**（`build.ps1` 交叉编译、`test-host.ps1` 主机测试、`package.mjs` 打包）。脚本里同时是唯一的**源文件清单与编译选项清单**，改动源文件后必须同步那里。
- 源码本身是 C++17，`src/` 面向 arm64 Android（交叉编译），主机侧只覆盖能在桌面编译的纯数据子集。工具链位置与主机编译器由脚本内部处理，**不属于本文档的约定**；换到其他机器或其他平台时以脚本为参考重建入口，见 §7。
- 下文凡出现 `app/…`、`third_party/…` 的路径，均**相对于参考实现仓库根**；凡出现 `src/…`、`tests/…`、`build/…` 的路径，均**相对于本仓库根**。
- 参考实现常用入口：
  - 配对/策略：`container/{TranslationJsonPairer,TranslationTextPolicy,TranslationSnapshot,TranslationIndexCompiler}.kt`
  - 原生运行时：`Foundation/LimbusTranslationRuntime.cpp`（5641 行）、`LimbusCodeIntegrityCompat.cpp`、`LimbusOwnedCode.h`

---

## 2. 现状：已验证 vs 未验证

### 2.1 有桌面证据（可复现）

| 能力 | 证据 |
|---|---|
| 纯数据层（配对、快照、索引编译/读取、文本策略） | `tests/*.cpp` 共 9 个文件，`all host checks passed` |
| arm64 交叉编译 | `bybus-zygisk.so` / `bybusd` / `bybusctl` 均 `machine=0xB7`，`-Werror` 零警告 |
| 模块打包 | 17 条目，`SHA256SUMS` 17/17 与 ZIP 内容一致，ZIP 摘要与 `.sha256` 一致 |
| 索引二进制布局 | `tests/index_format.cpp` 断言头部 22 字节布局；旧布局被拒（`index_shape`） |
| 方法签名解析 | `tests/type_signature.cpp` 断言 `AddMaterialReference` 解出 4 参、`[]&` 保留、泛型逗号不切错 |
| 文本质量门禁 | `tests/pairing_fidelity.cpp` 覆盖 blank/U+FFFD/C0-C1、display-array 逐项、十进制数值绑定 |
| 完整性口径 | `tests/integrity_parity.cpp` 覆盖零长 section/区间、256 条预算边界 |
| 快照缓存键 | `tests/cache_keys.cpp` 覆盖 `(generation, digest, bytes, content_bytes)` 四维 |
| 服务层边界 | `tests/bounds.cpp` 覆盖 AXML 长度前缀、签名块 `<24`、PT_LOAD `memsz` 越界 |

### 2.2 无任何证据（不要当成已知）

- L0–L3 实机矩阵：Root 基线、空模块、装钩、中文显示四层全部未记录。
- `has_inline_hook_space` 的方法地址表在真实 `libil2cpp.so` 上的命中率。
- `AddMaterialReference`、三个 ruby 类、`StoryData.GetScenario` 等新增 Hook 的真实签名命中与调用频率。
- TMP 字体/材质/shader ratio、富文本、注音（Ruby）在真实组件上的行为。
- AppSealing 兼容注入的实际检查时机与游戏反应（本地 CRC 与 `compatState=installed` **都不是**通过证据）。
- 设备侧 `/proc/self/fd/<n>` 是否被 Unity 字体加载器接受。

---

## 3. 本轮变更地图（按子系统）

> 每个子系统都遵循同一条原则：**参考实现的行为是权威**；移植侧与参考冲突时以参考为准。所有「有意偏离」集中在 §5。

### 3.1 文本与配对（`src/translation/`）

- `text_policy.{h,cpp}`：新增 `is_blank()`（Kotlin `String.isBlank()` 语义，空白集 = `Character.isWhitespace ∪ isSpaceChar`，**U+200B 不算空白**）与 `is_usable_display_text()`（拒 U+FFFD、除 `\n\r\t` 外的 C0、`0x7F..0x9F`，逐 UTF-16 unit 判定）。
- `json_pairer.cpp` 六处门禁：显示叶子、display-array 逐项、`released_texts` 两处、`count_fields` 两处；`stable_metadata` 改用同一 `is_blank`。
- 数值绑定：整数走 `int64` 精确比较，其余走十进制归一化；修掉 >2^53 的 id 误判相等。绑定遍历加 1,000,000 节点上限。
- ID 边界：`is_skill_number` 1..18 位；作用域标识与 story scene 名 1..96 字符。
- `json_pairer.h`：`sufficiently_paired(source, matched)` 提为自由函数，`PairResult` / `MergeResult` 都转发。
- `snapshot_builder.cpp`：统计过 90% 门槛的表数 → `available_source_file_count`；覆盖率不足时**不写** `index/index.bin`、`manifest.index.present=false`、`detail="insufficient_pairs"`，快照照常发布（对应参考「快照在、索引不激活」）。

### 3.2 索引（`src/translation/index_compiler.*`、`src/runtime/translation_index.*`）

**二进制布局已改为参考布局**（这是本轮的破坏性变更）：

```
偏移 0    6B   'L' 'Z' 'T' 'I' '1' 0x00
偏移 6    4B   大端 u32 schema = 9
偏移 10   4B   u32 uniqueCount
偏移 14   4B   u32 termCount
偏移 18   4B   u32 contextualCount
偏移 22   uniqueCount × (u32 len + 裸 UTF-8)(u32 len + 裸 UTF-8)
          termCount × 同上
          contextualCount × 同上
EOF（无尾部、无填充、无校验和）
```

其它对齐项：三个映射按**插入序**写出（`unique_order/term_order/contextual_order` + `put_ordered`，重赋值不移位）；`inputSha256` 输入序列改为 `path, context, field, source, translation, stable_id`（`context` 即参考的 `id`）；`add()` 门禁用 `is_display_field + is_blank + is_usable_display_text`；排序/去重键补 `context`；`@story:` 改严格整串 `^@story:[A-Za-z0-9_-]{1,96}(/dialog:[0-9]{1,18})?$` 且保序去重。

### 3.3 运行时装钩（`src/runtime/`）

- `type_signature.h`（新，header-only）：`normalize`（去空格、`/`→`.`、去反引号 arity、保留 `[]`/`&`）+ `split`（只按 `<>` 深度 0 的逗号切分，结构错误即判无效）。
- `il2cpp.cpp`：`find_verified_method` 改为「实际签名整串 vs 期望整串」比较；`method_get_class/method_is_generic/method_get_flags` 提为**必选**符号（缺一则所有 MethodSpec 静默失效）。
- `hooks.cpp`：
  - 装钩失败语义：**只有两个入口钩子**（`LoadLocalizeFileSync`、`TextAsset.get_text`）失败才回滚并回执 `unsupported_runtime`；其余（UGUI、TMP `set_text`/`set_text_string`/字体/材质、Announcer、剧情/RPG/选项/通知、富文本、ruby）失败只记日志。成功位 = 三个 trampoline 同时就绪（`load_localize && text_asset_text && tmp_set_text`）。
  - `AddMaterialReference` 单字面量（第 3 参 `TMPro.MaterialReference[]&`），删除永不匹配的 `alternate` 分支。
  - 三个 ruby 类同地址共享 trampoline（避免 `ledger.record` 区间重叠导致 slot 永久为空）。
  - 字体创建预算 3 次（原为一次性闩锁）。
  - 新增方法地址表 + `has_inline_hook_space`（**fail-open**，见 §5）。
  - 终局审计改调 `verify_owned_sections`，只写 `hook audit ...` 日志，不阻断。
- `zygisk_module.cpp`：degraded 分支补关 `index_fd`；`connect` 失败补关 socket；入口钩子未就绪 → `rejected` + `unsupported_runtime`。
- `state.h|cpp`：新增 `text_entry_installed()` 访问器（实现落在 `hooks.cpp`，因为两个原函数指针在匿名命名空间）。

### 3.4 完整性与读取

- `owned_code.h`：区间上限 512→**256**；允许零长区间与零长 section（`crc=0`、`owned=0`、返回 `true`）。
- `integrity.{h,cpp}`：新增 `verify_owned_sections()`（text+managed 共用累加器，`owned == substitute_count`，双 CRC 相等）。
- `snapshot_reader.{h,cpp}` + `cache_key.h`（新）：缓存键 `(generation, digest, bytes, 实际长度)`；`open()` 首行 `reset_cache()`；删除 `prewarmed_`，`prewarm_tables()` 改走同一条「先校验摘要才入缓存」的路径。
- `companion.cpp`：SCM_RIGHTS 超限描述符逐个 `close`；「收到数量 ≠ 头声明」直接失败（比之前严）。

### 3.5 服务层

`android_manifest.cpp`（长度前缀先判边界再读、限 4 字节续延）、`apk_signature.cpp`（`block_size < 24` 直接拒）、`native_alignment.cpp`（`memsz != 0 && memsz <= file_size && vaddr <= file_size - memsz`，重叠判定不再 u64 回绕）、`snapshot.cpp`（`rangesCount` 构建期上限 512→256）、`job_queue.cpp`（成功路径的 `detail` 也写进 `job status`）。

### 3.6 测试基建

- `tests/harness.h`（`BYBUS_TEST(name){...}` + `check(cond,"desc")`）、`tests/harness.cpp`（注册表）、`tests/test_main.cpp`（唯一 `main`）。
- `build/test-host.ps1` 改为编译 `tests/*.cpp` 全部 → **新增用例文件不需要改脚本**。
- 新增用例：`pairing_fidelity`、`index_format`、`type_signature`、`integrity_parity`、`cache_keys`、`bounds`。
- `tests/bounds.cpp` 内含 `translation::read_archive` 的**主机替身**（README 见 §8 陷阱）。

---

## 4. 必须守住的不变量（改动前先读这一节）

1. **索引头部顺序不可再变**：`magic + schema + 三个 count + 三组条目`。写入端与读取端、`manifest.index` 的三个计数与文件必须始终一致；不一致即整份清单被拒。
2. **文本门禁不可放宽**：任何新增的「把译文写进表格」的路径，都必须过 `is_blank + is_usable_display_text`，否则空串会清空游戏可见文本。
3. **装钩只对入口回滚**：新增可选 Hook 时不要引入 `restore_module`（`MSHookFunction` 不可撤销，回滚只会造成「指针非空但指向已还原字节」的混合态）。
4. **替换函数必须自带空指针回退**：因为可选钩子可能没装上，`g_original_*` 允许为空。
5. **构建期接受 = 运行期必须接受**：`snapshot.cpp` 的校验上限要与 `owned_code.h`、`translation_index.cpp` 的预算同源（§6.2 还有两处未同源）。
6. **诊断不写正文**：`logs/diagnostics.log` 与回执只允许阶段、计数、版本摘要、受限码；不写原文/译文/账号/设备标识。
7. **generation 发布后只读**，`active` 指针以 rename 原子切换。

---

## 5. 决策与「有意偏离」（共 6 条，附翻转条件）

| # | 偏离 | 原因 | 何时可以翻回 |
|---|---|---|---|
| 1 | `has_inline_hook_space` **fail-open**：地址表未命中只 `log_warn("hook space unknown")` 并继续装钩 | 采集范围（23 类 × 8 层祖先）尚无语料证明与参考等价；fail-closed 会在真机一次性关掉全部 Hook | L2 实机确认 `method addresses=` 数量与命中率后，改成 fail-closed 并加一条负向断言 |
| 2 | 收据词表**未扩**：不新增 `hook_setup_incomplete` | `shared/status.h` 的字符串集被视为冻结（加码需协议升版） | 若确需区分「部分生效」，升协议版本并同步 `webui/app.js` 的 `CODE_TEXT` |
| 3 | `inputSha256` 按参考算法计算但**不写入** `manifest.json` | 清单用 `index.sha256`（文件摘要）绑定索引，强于参考的输入摘要；避免格式扩张 | 若要与 Kotlin 侧产物做逐字节比对，可加可选字段 |
| 4 | `merge()` 不强制参考 `pair()` 的 `Unrecognized empty resource` | 移植侧单遍 merge 同时产记录与文档；若强制会让「0 显示字段的表」变成构建失败而不是被跳过 | 若出现「0 显示字段表」导致覆盖率虚高的实例，改为 pair 层先判 |
| 5 | 覆盖率按**包内并集**统计，不做参考的 per-locale 取最大值 | 本移植只处理单语言包 | 支持多语言包时按 locale 分别计数再取 max |
| 6 | `releasedTexts` 的目录产物仍**未被消费** | 字体覆盖判定在移植侧走 `font_proof.h` + 汉字区间启发式；完整 provenance 属 Phase 3 | Phase 3 接线时一并消费 |

**另一条需要知道的行为变化（不是偏离，是对齐）**：`hookState=installed` 现在表示「三个入口 trampoline 都在」，**不代表**所有可选钩子命中。已写入 `40-validation-matrix.md` §6。

---

## 6. 已知缺口

### 6.1 Phase 3（需单独批准，本轮明确未动）

1. 剧情/RPG/选项/通知的完整 `layout/flow/choice` 组件级证明状态机（参考 `LimbusTranslationRuntime.cpp:3020-3141`、`LimbusTextProvenance.h`、`LimbusFontProof.h`）。
2. `limbus_font_proof::Catalogue` 与 `text_provenance.h` 的零引用符号接线（`single_line_label`、`LayoutOverride`、`positioned_ruby`、`LabelHash` 等）。
3. `Style::ratios_off`（`RATIOS_OFF` 材质比例豁免）与 shader ratio 重绑钩子（参考 `:2603-2661`）。
4. `g_package_line_breaks` / `reflow_personality_title`。

> 这些是「头文件搬过来了但没接线」造成的假完成，`50-audit-findings.md` §3 第 9 条已登记。

### 6.2 两个「构建期接受 / 运行期拒绝」的分裂点

1. `snapshot.cpp` 用 `kMaxModuleBytes`（512 MiB）校验 `textVaddr/textSize`、`managedVaddr/managedSize`、`rangesVaddr`，而不是 `plan.module_size`。描述符表落在真实映射之外、但仍 < 512 MiB 的清单能进快照，运行期按真实映射必然失败。
2. `rangesFingerprint` 构建期只校验形状（16 位 hex），没有用 `appsealing_profile.cpp` 已有的 `appsealing_range_fingerprint(items, count, 24)` 与清单值比对。

### 6.3 工程质量项（与参考行为无关，未排期）

- `CommandService` 的每个 handler 由 `IpcServer` 的**每连接一线程**并发调用，而 `commands.cpp` 无锁；`status()` 可读到跨子系统不一致的快照。
- 多处持锁做 IO：`job_queue` 的 `submit/view/cancel` 在 `mu_` 内 fsync/读盘；`diagnostics::record_text` 持锁全量重写；`GameProbe::snapshot` 持 `probe_mu_` 里 spawn `pm/dumpsys`。
- `IpcServer` 连接线程回收会 join 仍在运行的连接；worker 与连接线程都无异常边界（`bad_alloc` 触发 `std::terminate`）。
- `fs.cpp` 的 `remove_tree` 硬编码 `/data/adb/bybus`（换 `state_root` 后清理变静默 no-op）；`path_is_inside` 不解析符号链接；`write_file` 无 `O_NOFOLLOW`。

---

## 7. 操作手册

`build/` 下这套脚本是**Windows 下编译用的**构建入口（`build.ps1` / `test-host.ps1` / `package.mjs`）；它们同时也是源文件清单与编译选项的唯一来源。Linux 下暂无等价入口，见本节末尾。

```text
cd <仓库根>

# 1) 主机测试（编译 tests/*.cpp 全部 + 链接纯数据层）
build\test-host.ps1
#    通过时出现两行，缺一不可：all host checks passed / host smoke passed

# 2) arm64 交叉编译（-Wall -Wextra -Werror，警告即失败）
build\build.ps1 -Target all
#    -> build\out\bybus-zygisk.so / bybusd / bybusctl

# 3) 打包（会重建 WebUI 静态资源并生成 SHA256SUMS）
node build\package.mjs
#    -> build\bybus-1.0.0.zip（17 条目）+ build\SHA256SUMS + *.sha256

# 4) 清理 build\obj 与 build\out
build\build.ps1 -Target clean
```

**换平台要做什么**：目前没有 Linux/CMake 入口。新增入口时不要凭印象重写，逐项对齐 `build/build.ps1` 与 `build/test-host.ps1` 里已有的三样东西——源文件分组（`$sharedSources` / translation / service / runtime / control）、每组使用的编译选项（自家代码 `-Wall -Wextra -Werror`；vendored 代码单独静音）、以及产物路径（`build/out/bybus-zygisk.so`、`bybusd`、`bybusctl`）。`package.mjs` 的输入是 `module/` 与 `build/out`，与平台无关，可复用。

本轮验收值（重新打包后会变，仅作对照）：ZIP `sha256 = b3a55d769c86a4e141bf6fe492b0eda438d8de61caceab1dd66b0744679b9747`，17 条目全部与 `build/SHA256SUMS` 吻合。

---

## 8. 陷阱清单（踩过或明确标注的）

1. **`tests/bounds.cpp` 里有 `translation::read_archive` 的主机替身**。若把 `src/translation/archive_reader.cpp` 加入主机构建，必须先删替身，否则重复定义。
2. **索引布局变更 = 破坏性迁移**。升级后旧 generation 的 `index.bin` 读不出来 → 回执 `code=index_invalid` → 必须在设备上执行一次 `snapshot build` 重建。`index_invalid` 是 receipt 的自由字符串码，**不在** `shared/status.h` 的协议码词表里。
3. **prewarm 与按需读取共用 32 MB / 32 条 LRU**。表多或总量大时先预热者会被淘汰，首次命中会回退到同步 IPC + 读盘 + 哈希（延迟代价，正确性不变）。`50-audit-findings.md` §4 已登记。
4. **不要用 `git diff` 判断本轮改了什么**：仓库在本轮之前已有大量未提交修改（`hooks.cpp`、`il2cpp.*`、`state.*`、`text_router.*`、`zygisk_module.cpp`、`snapshot.*`、`webui/app.js` 等），`git diff` 会把两批混在一起。以 `50-audit-findings.md` 与本文 §3 为准。
5. **`find_method` 与 `find_verified_method` 强度不同**：前者只校验名字 + 返回类型 + 参数个数，后者校验声明类、参数类型、返回类型、泛型、静态性。新增 Hook 一律用后者。
6. **新增测试文件不要写 `main`**，用 `BYBUS_TEST(...)`；`test_main.cpp` 已有唯一 `main`。
7. **`build/` 下大部分产物被 gitignore**（`build/obj/`、`build/out/`、`build/bybus-*.zip`、`module/bin|zygisk|webroot`）。它们不是源码，不要提交。
8. 文档里的产物路径均相对仓库根；工具链位置与主机编译器由 `build/` 脚本自行解析，不要在文档、脚本或注释里写死任何机器相关路径。新增平台入口时以 `build/build.ps1` 的源文件清单与选项为基准（§7）。

---

## 9. 仓库状态与提交建议

```
branch: main
HEAD  : 7970382 Add SIGSEGV guard and TMP font switching
改动  : 43 修改 + 13 新增（共 56），全部未提交
新增  : src/runtime/{type_signature.h, cache_key.h, library_watch.cpp, library_watch.h}
        tests/{harness.h, harness.cpp, test_main.cpp, host_smoke.cpp(改), pairing_fidelity.cpp,
               index_format.cpp, type_signature.cpp, integrity_parity.cpp, cache_keys.cpp, bounds.cpp}
```

建议按四批提交（每批都能独立编译、独立跑通主机测试）：

1. `tests: harness + glob wiring`（`tests/harness.*`、`tests/test_main.cpp`、`build/test-host.ps1`、`host_smoke.cpp` 的机械改造）
2. `translation: reference-parity gates, decimal binding, index layout`
3. `runtime: signature normalization, partial-install semantics, integrity audit, cache keys`
4. `service: bounds hardening + docs/handoff`

> 提交前请确认 `build/SHA256SUMS` 与 ZIP 一致，并重跑 §7 的三条命令。

---

## 10. 下一步（按序，含验收口径）

1. **上机做 L0–L3**：按 `40-validation-matrix.md` 逐层记录（每行一个 `gameVersionCode + libil2cpp.so SHA-256`）。L2 必须看到 `hookState=installed`、`compatState=installed`、`survivedMillis ≥ 15000`、日志无 `AddMaterialReference trampoline missing`、`hook audit owned=N/N`。
2. **按实机日志复核 `has_inline_hook_space`**：`method addresses=` 的数量、`hook space unknown` 出现频率。命中率可接受后把 fail-open 翻成 fail-closed（§5 偏离 1）。
3. **补 §6.2 的两个分裂点**：`snapshot.cpp` 改为按 `plan.module_size` 校验地址；构建期用 `appsealing_range_fingerprint` 交叉核对 `rangesFingerprint`。两条都应先加 host 用例再改。
4. **Phase 3 分两批**：先 `text_provenance` / `layout` 判定（可 host 测的部分先测），再字体 provenance（`Catalogue`）与材质比例。
5. **服务层并发模型**（§6.3）：优先做「锁内不做 IO」与「`CommandService` 单一入口串行化」，其余按需。

---

## 11. 本轮实施分工留痕（便于追溯）

| 任务 | 范围 | 结论 |
|---|---|---|
| T0 | 测试基建（harness/glob） | 完成，`host smoke passed` |
| T1 | 文本门禁 + 配对 + 覆盖率 | 完成；报告 3 处口径差异（已并入 §5 第 4/5 条） |
| T2 | 索引布局/摘要/排序/`@story:` | 完成；旧布局被拒实测 |
| T3 | 签名归一化 + 装钩语义 + 内联空间 + 字体预算 | 完成；`restore_module` 仅剩入口两处 |
| T4 | `owned_code` 口径 + `verify_owned_sections` | 完成；负向对照可区分修复前后 |
| T5 | 缓存键 + companion fd | 完成；报告 prewarm 预算代价（§8 陷阱 3） |
| T6 | AXML / 签名块 / PT_LOAD 边界 | 完成；AXML 越界读不可在无 ASan 下区分，属行为回归保护 |
| T7 | `host_smoke` 断言翻转 | 完成；纠正了「超长 skill id 的 `stable_id` 等于 id」这一错误预期（实际为空串，已按参考行为断言） |
| T8 | 文档同步 | 完成；README / 50-audit / 10-port / 01-design / 40-matrix / 30-build 全部对齐 |
| T9 | 收口两处一致性缺口 | 完成；`rangesCount` 256、`job.detail` 可读 |
