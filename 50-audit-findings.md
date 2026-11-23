# 代码清查结果

日期：2026-11-23。范围：`ksu-native-design` 的内容管线、原生运行时、WebUI、后台协议及模块安装链路。静态审查与修复已完成，随后通过主机测试、arm64 全量编译和模块打包。**没有 Android 实机验证。** 原始仓库 `D:\BYBUS\source` 保持独立。

## 确定问题（审查时）

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

## 修复状态

- **内容管线已修复**：解除 JSON 输入、单表及 manifest 的旧固定大小上限；游戏原文表读取或目录枚举失败现在明确报错，避免静默发布缺表快照；技能上下文不再在 256 字符处截断。`tests/host_smoke.cpp` 覆盖超过旧阈值的内容与长技能 ID。
- **后台与安装已修复**：任务恢复扫描超过 512 个文件并保留最近终态；历史任务查询不污染最新任务指针；去重键加入 `activate`；IPC 停止标记和 socket 生命周期消除已发现的数据竞争；卸载只通过私有控制 socket 请求守护进程退出，不再对旧 PID 直接 `kill`。
- **运行时已修复**：短时正常退出和并发启动不再自动产生永久降级标记；字体 SHA-256 在安装 Hook 前由监控线程验证；非关键 receipt 写入移出 Unity 回调；汉字判断加入 UTF-16 代理对处理。`webui/app.js` 已更正 PAT 权限提示，并在页面说明同源存储限制。
- **原版 AppSealing 注入已移植**：新增 `src/runtime/appsealing.cpp`/`.h`、`src/runtime/owned_code.h`、`src/translation/appsealing_profile.cpp`/`.h`。快照的 `profile.appsealing` 由 `bybusd` 从本机 APK 结构资格生成（`libcovault-appsec.so` 与 `libil2cpp.so`），运行时按计划安装 JNI detach 兼容点、50048 报告桥、70034/50040 站点补丁、471 瞬时 50048 维护与 SIGILL 守卫；补丁账本同时覆盖两个库；任一步失败即回滚并回执 `unsupported_runtime`。`profileState` 增加 `unsupported`，receipt 与 `diagnostics.runtime` 增加 `compatState`（`unknown|none|installed|rejected`）。本轮没有 arm64 设备，未做任何实机验证。
- **本机验证已通过**：host smoke、`webui/release.test.mjs`、`node --check webui/app.js`、NDK arm64 全量构建与模块 ZIP 打包。主机测试不等同于游戏进程和设备验收。

## 待验证的风险

- `src/runtime/hooks.cpp:126`、`src/runtime/snapshot_reader.cpp:130`：首次命中大表时在回调线程同步 IPC、读盘并计算 SHA-256；若回调位于 Unity 主线程，可能产生卡顿。`try_enable_translation` 也会同步等待 companion。
- 配对深度 64 层仍沿用原实现；需要真实汉化包与游戏原文做差分验证。该规则不限制 70+ MiB 总正文。
- 已保存的 PAT 位于 KernelSU 管理器 WebView `localStorage`。当前 KernelSU 管理器的模块 WebUI 共享 `mui.kernelsu.org` 来源；其他模块页面理论上可读取此键。应只使用专用、无额外权限的 PAT。安装的模块本身拥有 root 桥接，此存储并不提供跨模块隔离。

## 验证边界

- 当前构建与 host smoke 测试通过；这只能证明编译、打包和纯数据层样例，不覆盖上述边界情况。
- `40-validation-matrix.md` 的 L0–L3 仍全为“未验证”。Zygisk Hook、AppSealing 反馈、语言资源是否重下、字体路径和 WebUI 设备行为均不构成通过依据。
- 本轮已重新生成 ZIP，但尚无设备，WebUI、Hook、字体、游戏保护和资源下载行为均未验证。原版 AppSealing 注入已完成移植：报告桥、70034/50040/50048 站点、471 瞬时维护、SIGILL 守卫与双库账本都在 `src/runtime/appsealing.cpp`，计划由 `bybusd` 从本机 APK 生成；这些改动只经过桌面构建与静态审查，尚无任何 arm64 实机记录，保护检查的实际时机与反应仍需设备观察。
