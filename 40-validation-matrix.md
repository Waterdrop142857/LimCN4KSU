# 实机验证矩阵

版本：2026-11-24。本文件是 `01-implementation-design.md` §6 与 `12-kernelsu-appsealing.md` §3 的落地记录表。**当前所有单元格都是“未验证”**：本文档环境没有 arm64 设备，桌面测试只能覆盖纯数据层（见 `30-build-and-install.md` §3）。

## 1. 记录规则

- 每一行对应一个 `gameVersionCode + libil2cpp.so SHA-256` 组合；换版本必须新开一行，不得沿用旧结论。
- 四层必须按 L0→L3 顺序各自记录，任何一层失败即停止更高层，不把“注入后可启动”当成“汉化可用”。
- 只记录阶段、计数、错误码与时间；不记录原文、译文、账号或设备标识。
- 出现安全提示或进程退出时，把该行标为 `blocked` 并写明层号，先修该层再继续。
- 索引是否就位、资格是否通过、守卫是否匹配属于每一行的固定观察项，不能只在备注里口头说明。

## 2. 分层定义

| 层 | 配置 | 通过标准 |
| --- | --- | --- |
| L0 | 不安装本模块 | 官方包在既有 Root 环境下可正常游玩（设备基线） |
| L1 | 安装模块，Zygisk 可用，快照为空 | 游戏到达标题页；`status.snapshot.state=missing`，无安全提示 |
| L2 | 有快照，`profile` 与 `index` 都存在，AppSealing 兼容注入 + 文本 Hook 安装 | `runtime.compatState=installed` 且 `runtime/hookState=installed`，`survivedMillis ≥ 15000`（receipt `confirmed=true`），无安全提示；同时满足 §4 的索引与守卫项 |
| L3 | L2 + 字体就绪 + 单表中文显示 + 索引查表生效 | `fontState=ready`，指定 UI 表显示中文，`hits>0`；散文/术语/上下文三类索引查表各至少命中一次；同时核对 `compatState` 仍为 `installed`，没有 50048/70034/50040 上报或保护提示 |

分层里新增的两项判据：

- **索引**：`L2` 要求 `manifest.json` 有 `index` 且 companion 交付了第三个描述符；没有索引的旧 generation 只能记作“无索引降级”，不算 L2 通过。`L3` 要求索引内容可用（查表命中），不只是加载成功。
- **资格与守卫**：`L2` 要求构建期资格检查通过（`job status` 的 `detail` 不含 `qualification_*`），并且 `compatState` 不是因 `appsealing_guard_mismatch` 被置为 `rejected`。守卫字不匹配属于该版本不匹配，必须换版本行重测，不能通过放宽检查绕过。

## 3. 记录表

| # | gameVersionCode | libil2cpp SHA-256（前 12 位） | L0 | L1 | L2 | L3 | 备注 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 待填 | 待填 | 未验证 | 未验证 | 未验证 | 未验证 | 首个实机里程碑 |

## 4. 每层需要同时核对的项目

L2/L3 通过时，逐条勾选：

1. `bybusctl status` 的 `game.textState=ready`、`game.tableCount>0`。
2. `game.profileState=ready` 且 `game.profileCode=ok`；出现 `unsupported` 即该行未通过（`unsupported` 表示该 APK 声明了保护库但清单里没有合格计划，运行时按设计拒绝 Hook，`profileCode=unsupported_runtime`，该行只能记为未通过）。
3. `diagnostics.runtime.compatState=installed`；`rejected` 说明兼容注入中途失败并已回滚，须按进程日志里的理由（如 `appsealing_guard_mismatch`、`appsealing_site_mismatch`、`appsealing_descriptor_fingerprint`）修计划后重测。
4. `diagnostics.runtime.generation` 与 `snapshot.generation` 一致。
5. 构建任务的 `detail` 为空或不含 `qualification_*`；SDK、ABI、PT_LOAD 对齐、签名四项都通过。
6. 当前 generation 目录下 `manifest.json` 的 `index.sha256`/`bytes`/三个计数与 `index/index.bin` 实际一致；进程日志没有 `snapshot_index_missing` 或 `snapshot_index_invalid`。
7. 快照前后官方语言文件与下载清单的摘要不变（`sha256sum` 对比），且没有重复下载同一资源。
8. 关闭再打开 WebUI 能恢复任务进度；`job status` 终态正确。
9. 切换来源后显示“下次启动生效”，且运行中的进程仍用启动时的 generation。
10. 构建中断（杀 `bybusd`）后重启：`imports/`、`generations/.tmp-*` 被清理，`active` 仍指向上一完整快照。

## 5. 失败时的判定

| 现象 | 先查 | 可能结论 |
| --- | --- | --- |
| 启动即退出，无提示 | `runtime/receipt.json` 的 `hookState/confirmed` 与设备日志 | receipt 只记录观察结果，正常退出与崩溃在该文件中没有区分依据；不得据此自动永久禁用 Hook |
| 出现安全提示 | `diagnostics.runtime.compatState`、L2 是否有合格 `profile`、补丁账本是否完整 | 该版本未通过实机兼容验收；停止宣称支持并保留诊断 |
| `compatState=rejected` 且日志是 `appsealing_guard_mismatch` | 计划的 `guards` 与活体库指令字 | 该安装包不在计划家族内；换版本行重测，不放宽守卫 |
| 中文显示为方框 | `diagnostics.runtime.fontState` | TMP 字体注册或 `HasCharacter` 探测失败 |
| 表未汉化 | `diagnostics.log` 的 `pairing` 计数 | 原文摘要未命中（加载器改变了字节表示），该表回退原文 |
| JSON 表正常但散文/术语不汉化 | 进程日志 `snapshot rejected: snapshot_index_*`、`manifest.json` 的 `index` | 索引缺失或不一致；重建快照后重测 |
| `profileState=missing` | `build/` 日志与 APK 路径 | `pm path` 未取到 base.apk 或 ELF 解析失败 |
| 任务失败且 `detail=qualification_*` | `job status` 的 `detail` | 设备/安装包不满足 SDK、arm64、PT_LOAD 对齐或官方签名资格 |

## 6. 当前已知未验证项

- 游戏原文目录是否为 `<files>/Localize/<jp|en|kr>`，以及加载器是否改变 JSON 字节表示。
- `/proc/self/fd/<n>` 是否被 TMP/TrueType 加载器接受为字体路径。
- KernelSU 管理器 WebUI 的 `kernelsu.exec` 调用形态，以及 root 守护进程读取本地绝对路径的能力。
- AppSealing 兼容注入已移植（JNI detach 兼容点、50048 报告桥、70034/50040 站点补丁、471 瞬时 50048 维护、SIGILL 守卫、双库补丁账本、`guards` 复核），实际检查时机与反应仍需设备观察；本地 CRC 比对与 `compatState=installed` 都不构成通过证据。
- 索引的实机行为：schema 9 二进制是否能被运行时完整读入、`index/index.bin` 的 64 MiB 上限在真实字典量级下是否够用、第三个描述符在 Unity 加载器压力下是否稳定，全部未测。

## 7. 尚未接入、因此不在本矩阵考核范围的部分

以下能力没有接进运行时，验证矩阵不为它们设通过标准，也不能把它们的缺失读成“该层失败”：

- `font_proof.h`、`flow_text_proof.h`、`font_materials.h`、`font_scale.h`、`text_provenance.h`：TMP 材质/字号/多语言字体 setter、富文本与注音 Hook 均未安装。
- 剧情/RPG/选项/通知只做通用字符串字段与参数翻译，原版 layout/flow/choice 状态机未移植。
- `android_dlopen_ext` watcher 未实现，仍为 100 ms 轮询 `libil2cpp.so`。
- coverage/source 备份/图标/更新链接等应用级管理未实现。
- 尚无 arm64 实机 L0–L3 记录：本文件任何一行都不能被引用为“已通过”。
