# KernelSU 运行环境与 AppSealing 兼容设计

版本：2026-11-23。约束：模块只在 KernelSU 上运行；游戏带 AppSealing（`com.inka.appsealing.AppSealingApplication` + `libcovault-appsec.so`）。本文件补充 `01-implementation-design.md`，不替代它。

## 1. KernelSU 事实与模块落点

| 事实 | 本模块的处理 |
| --- | --- |
| 模块目录 `/data/adb/modules/bybus`，安装后由 KernelSU 解析 `module.prop` | 只使用 `module.prop`、`service.sh`、`post-fs-data.sh`、`action.sh`、`uninstall.sh`、`webroot/`、`bin/`、`zygisk/`、`assets/` |
| KernelSU 无内置 Zygisk | 声明外部依赖 ZygiskNext；`status` 读取 `/data/adb/modules/zygisksu/module.prop` 与 `disable` 标记给出 `zygisk.available`；模块不打包、不安装、不修改 ZygiskNext |
| 脚本以 root 运行，`/data/adb/modules/<id>` 可写，应用进程不可读 | 持久状态放 `/data/adb/bybus/`（root 700）；游戏进程不按路径读取，字体与快照用 companion 交付的文件描述符 |
| 模块 WebUI 通过官方 `kernelsu` npm 库调用 `exec` | `bybusctl` 是唯一入口，路径固定为 `/data/adb/modules/bybus/bin/bybusctl`；页面不直接调用 `bybusd` |
| 模块升级会替换模块目录 | 已提交归档与快照保存在 `/data/adb/bybus/`，升级不清空；`uninstall.sh` 只清理确认属于本模块的目录 |
| 禁用模块 | `post-fs-data.sh` 不启动 `bybusd` 时不注入；游戏按既有 Root 基线运行 |

`bybusd` 由 `service.sh` 在晚启动阶段拉起，实例锁由守护进程持有。`post-fs-data.sh` 清理未提交导入、临时 generation 与失效 socket。

## 2. AppSealing 检查面

已确认的保护组件与检查面（来自 `source/third_party/virtualapp-upstream/lib/src/main/jni/Foundation/LimbusCodeIntegrityCompat.cpp`、`LimbusIntegrityLayout.h`、`helper/compat/LimbusNativeProfile.java`）：

1. `libil2cpp.so` 的 `.text` 段 CRC32。
2. `libil2cpp.so` 的托管元数据段（managed 段）CRC32。
3. 保护库报告桥：命中既定调用点时以 5 位错误码（如 50048）上报并终止进程。
4. 官方签名与包身份：官方证书 SHA-256 `9ea88fdc27195dfef99050f88a889cad86cdf1a80faca2e22d6464d47f973145`；`Application` 类名等于 `com.inka.appsealing.AppSealingApplication` 时视为声明保护。

不改变游戏文件的方案仍然会改动 `libil2cpp.so` 内存里的代码：IL2CPP 方法内联 Hook 会写入 4–20 字节。本地 profile 与补丁账本只能决定是否尝试 Hook，不能证明保护组件的 CRC 或报告桥会接受这些改动；具体反应必须实机观察。

## 3. 分层策略（fail closed）

| 层 | 内容 | 判定 |
| --- | --- | --- |
| L0 | 不注入 | 设备基线：Root + 官方 APK 可正常游玩 |
| L1 | 空 Zygisk 模块（不 Hook、不读快照） | 必须到达标题页，否则本模块在该版本上不可用 |
| L2 | 先按快照 `profile.appsealing` 安装 AppSealing 兼容（JNI detach 兼容点、50048 报告桥、70034/50040 站点补丁、471 瞬时 50048 维护、SIGILL 守卫），再 Hook `TextAssetPatchManager.LoadLocalizeFileSync`、`UnityEngine.TextAsset.get_text`；字体 API 可用时还 Hook `UnityEngine.UI.Text.set_text` | 声明了保护库的版本须 `runtime.compatState=installed`（无保护库时为 `none`），且 `runtime.hookState=installed`；文本 Hook 只做读后 JSON 替换与字体选择，不触碰资源文件描述符、下载、存档 |
| L3 | 完整性结果观察 | 在 L2 后记录安全提示、退出和重复下载；报告桥与站点补丁已移植，`compatState` 须保持 `installed`，且没有 50048/70034/50040 上报 |

**安装顺序**（`src/runtime/appsealing.cpp` 的 `install_appsealing_compat`，在 `zygisk_module.cpp` 的监控线程里先于任何文本 Hook）：

1. 定位 `libcovault-appsec.so` 并读取 `profile.appsealing`。快照声明了保护库但没有合格计划时，`compatState` 置为 `rejected`、回执 `unsupported_runtime`，文本 Hook 一次都不安装。
2. 复核活体库：模块映射、范围描述符数量与指纹、每个站点的原始指令字、detach 入口字节必须与计划一致，否则以对应的 `manifest_appsealing_*`/`appsealing_*` 理由失败。
3. 安装 JNI detach 兼容点：核对调用者地址、调用栈上的 `JNIEnv` 与描述符索引后跳到计划给出的 continuation，不改变容器或文件描述符语义。
4. 安装 50048 报告桥：仅在错误码、调用者地址与指令模式同时匹配时跳过上报与终止。
5. 逐站点写 70034/50040 补丁并计入补丁账本；471 的瞬时 50048 站点由维护线程按 `branchWord` 重写，SIGILL 守卫按 `sigillPcVaddr`/`sigillLrVaddr` 复位 PC。
6. 以上全部成功才置 `compatState=installed`，之后才安装文本 Hook。

**清单计划字段**：计划由 `bybusd` 从本机 APK 生成（`src/translation/appsealing_profile.cpp`），写在 generation 的 `manifest.json` 的 `profile.appsealing` 下：`family`、`protectionSha256`、`moduleSize`、`textVaddr`/`textSize`/`textCrc`、`managedVaddr`/`managedSize`/`managedCrc`、`rangesVaddr`/`rangesCount`/`rangesFingerprint`、`sites[]`（`kind`、`vaddr`、`targetVaddr`、`expected`、`word`、`substitute`）、`detach` 与 `report` 的入口地址与期望指令数组、`detachCallerVaddr`/`detachContinuationVaddr`/`confirmCallerVaddr`/`confirmContinuationVaddr`、`stack`（记录与调用栈偏移，含 `descriptorCountOffset`、`descriptorEntriesOffset`、`descriptorStride`）、`transient`（`vaddr`、`targetVaddr`、`expectedWord`、`branchWord`、`sigillPcVaddr`、`sigillLrVaddr`）。字段缺失或越界按 `manifest_appsealing_*` 拒绝整份清单，运行时只用通过校验的字段。

**失败回滚**：detach Hook、报告桥与每个站点补丁都是先 `ledger.record`、写入成功后 `ledger.commit`；任一步失败立即 `restore_module()` 只还原保护库已写字节、清空原函数指针与保护库基址、`compatState` 停在 `rejected`，并跳过后续站点与文本 Hook，回执写 `hookState=rejected`、`code=unsupported_runtime`。同一进程内 `rejected` 不再重试，也不会退化成“部分补丁”继续运行。

规则：

- profile 只能来自本机证据。`bybusd` 在构建快照时从已安装 APK 里解析 `lib/arm64-v8a/libil2cpp.so` 与 `libcovault-appsec.so`，得到 `.text` 段的 offset、size、CRC32，写入 generation 的 `manifest.json`，并绑定 `gameInstallIdentity`。运行时不接受来自网络或用户输入的 profile。
- profile 与运行时观察到的实际段布局不符（offset/size/CRC 任一不符）时，**不安装 Hook**，返回原文，写 `unsupported_runtime`。
- 补丁账本记录每个 Hook 写入前后的字节，供本模块本地校验与失败回滚。归一化 CRC 只用于核对自身记录，不改变 AppSealing 实际读取到的内存；当前实现不能据此推断保护检查会通过。
- `LimbusIntegrityLayout.original_expectations` 的语义保留：如果本机 profile 只有布局没有原始期望值，则不伪造期望值，也不把“当前活体 CRC”当作期望值。
- 字体文件 SHA-256 在 Hook 安装前的监控线程验证；缺失或不符时跳过 Hook。Unity/TMP 字体对象仍在游戏主线程首次文本回调时建立。首次命中未缓存的大表仍会在回调线程同步读取并校验，须实机测量卡顿。
- 容器相关部分不移植：VirtualApp 的容器重定向、`IORelocator`、`MapsRedirector` 与 `syscall` Hook 都不进入本运行时。报告桥本身已移植，但 `LimbusCodeIntegrityCompat.cpp` 里的报告桥常量（如 `0x4e568`、错误码与 continuation 偏移）不写死为通用逻辑：它们属于特定版本家族，必须由 `profile.appsealing` 提供，缺失即关闭。

## 4. 启动结果记录

Zygisk companion 为最近一次游戏启动维护一条 receipt（`/data/adb/bybus/runtime/receipt.json`）：

```json
{ "schema":1, "generation":"<64 hex>", "startedAtMillis":0, "hookState":"none|installed|rejected",
  "fontState":"none|ready|rejected", "compatState":"none|installed|rejected",
  "hits":0, "misses":0, "code":"ok", "survivedMillis":0 }
```

- 游戏进程在 Hook 安装后由监控线程写 receipt，此后后台每 5 秒更新 `survivedMillis`、`fontState` 和计数。进程退出时没有可靠的最终回写，因此 `confirmed=false` 只表示尚无满 15 秒的观察记录，不能判断为崩溃。不会据此自动创建降级标记；仅 `degrade.json` 中当前 generation 且 `reason=manual` 的显式标记可禁用 Hook。
- `compatState` 取自进程内的 AppSealing 兼容状态：`none` 表示没有安装（含未声明保护库的版本），`installed` 表示 detach 兼容点、报告桥与站点补丁全部就位，`rejected` 表示安装中途失败并已回滚。它只描述本模块做了什么，不证明保护检查已通过。
- `diagnostics` 只暴露阶段、计数、`compatState` 与 `code`。
- L0–L3 四层结果按 `gameVersionCode + libil2cpp.so SHA-256` 记在 `40-validation-matrix.md`，不把某一层的成功当作全量兼容证明。

## 5. 与设计文档的张力

`01-implementation-design.md` §6 要求“现有 AppSealing 处理不作为移植完成证据”。原版的保护组件兼容注入已按本机快照移植：JNI detach 兼容点、50048 报告桥、70034/50040 站点补丁、471 瞬时 50048 维护与 SIGILL 守卫都在 `src/runtime/appsealing.cpp`，补丁账本同时覆盖 `libil2cpp.so` 与 `libcovault-appsec.so`。移植完成不等于兼容性成立：目前仍无 arm64 实机记录，保护检查的实际时机与反应必须在设备上按 L0–L3 四层观察，本地归一化 CRC 不能当作 AppSealing 通过的证据。
