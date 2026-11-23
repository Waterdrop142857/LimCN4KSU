# BYBUS KernelSU 原生汉化移植实现设计

版本：2026-11-23。依据 `D:\BYBUS\source` 的提交 `bd3260848029ed41558da4f40a9c9edf6f622dd6`。本文是实现设计，尚未在真实设备上验证 Zygisk 注入后的游戏表现。

## 1. 目标与边界

官方 `com.ProjectMoon.LimbusCompany` 安装包直接在原生 Android 环境运行。用户已确认该设备在 Root 环境、无代码注入时可以正常游玩。交付物为 KernelSU 模块 ZIP，管理入口是 KernelSU WebUI。模块支持现成的都市零协会汉化包及兼容 ZIP、7z 本地文本包，启动前针对设备上已有的游戏原文生成汉化快照。已下载的新原文在重新生成快照并重启游戏后生效。

本阶段保留游戏自己的资源文件、下载清单、登录和账号数据。运行时只在游戏文本读取函数返回后，按资源身份及原文摘要替换内存中的 JSON 字符串。缺少匹配快照时返回原结果。AI 生成、在线逐条翻译、运行中重新配对和首次下载后的立即汉化不进入首版。BYBUS 现有实现的读后合并和文本配对是行为基准。[源码依据](../source/app/src/main/java/com/example/limbuszhcn/container/TranslationJsonPairer.kt) · [原生读取入口](../source/third_party/virtualapp-upstream/lib/src/main/jni/Foundation/LimbusTranslationRuntime.cpp)

设备前提是 arm64、KernelSU 与可用的 Zygisk 实现。KernelSU 本身没有内置 Zygisk，模块声明外部依赖并在 WebUI 中报告检测状态。模块不打包或重新分发 ZygiskNext。发布和安装过程不要求完整 Android SDK，原生可执行文件与 `.so` 使用 NDK 编译，WebUI 是静态资源。[KernelSU FAQ](https://kernelsu.org/guide/faq) · [ZygiskNext 项目](https://github.com/Dr-TSNG/ZygiskNext) · [KernelSU WebUI](https://kernelsu.org/guide/module-webui.html)

## 2. 产物及模块边界

建议在新的实现目录中建立以下结构。此目录与原仓库 `D:\BYBUS\source` 保持独立；复制或提取原仓库代码时记录来源与许可证。

```text
module/
  module.prop
  service.sh
  webroot/
    index.html
    app.js
    app.css
  bin/
    bybusctl                 # arm64，WebUI 的固定命令入口
    bybusd                   # arm64，后台任务与只读原文访问
  zygisk/
    arm64-v8a.so             # 仅进入官方游戏主进程
  assets/
    ChineseFont.ttf.xz       # 按原项目许可和 SHA-256 核验
```

持久状态、归档与快照保存在模块安装目录之外的专用私有目录，例如 `/data/adb/bybus/`，避免模块升级替换目录时丢失。该目录由后台服务创建并按固定权限管理。卸载时由明确的清理动作处理，不能让安装脚本误删用户导入包。

`webroot/index.html` 是 KernelSU 模块 WebUI 的入口。模块元数据使用 `module.prop`；本方案不覆盖 `/system` 文件，也不需要系统挂载型 metamodule。WebUI 调用固定的 `bybusctl` 子命令，命令返回 JSON；下载和编译由后台任务执行。WebUI 关闭后任务仍由后台服务管理，持久状态写入模块数据目录。[KernelSU 模块文档](https://kernelsu.org/guide/module.html) · [WebUI 文档](https://kernelsu.org/guide/module-webui.html)

模块间职责如下。

| 组件 | 输入 | 输出 | 约束 |
| --- | --- | --- | --- |
| WebUI | 用户操作、控制程序 JSON | 页面状态、任务进度 | 不直接修改游戏文件或编辑持久状态 |
| `bybusctl` | 固定子命令及有限参数 | 稳定 JSON 响应 | 校验所有参数，经本地 IPC 提交任务 |
| `bybusd` | 汉化归档、只读游戏原文 | 已校验包、不可变快照、诊断 | 原子发布，不写游戏资源目录 |
| Zygisk 模块 | 进程身份、快照只读句柄、原方法返回值 | 合并后的内存字符串 | 只在游戏主进程启用，失败时交还原值 |

## 3. 控制面协议

WebUI 仅调用 `status`、`sources list`、`sources select <id>`、`update start`、`import begin/chunk/commit/abort`、`snapshot build`、`job status <id>`、`diagnostics` 等固定命令。`sources select` 修改待激活源，新的快照仅在下次游戏进程启动时加载。每次请求返回 `schema`、`ok`、`code`、`message`、`data`。错误码固定，UI 不解析自由格式日志。

`bybusctl` 将网络请求、解包、哈希和快照编译提交给 `bybusd`，立即返回任务 ID。`job status` 给出阶段、已完成项、总项和错误码。重复提交同一版本与同一源时复用已完成快照；不同源的任务互不覆盖。进程中断后，后台服务清理未提交的临时目录并保留最后一个完整快照。

控制程序的响应示例固定为 `{"schema":1,"ok":true,"code":"accepted","message":"","data":{"jobId":"..."}}`。任务状态取 `queued`、`fetching`、`validating`、`pairing`、`publishing`、`done`、`failed`、`cancelled`。错误码至少区分 `game_missing`、`text_missing`、`archive_invalid`、`source_changed`、`font_invalid`、`snapshot_invalid` 和 `unsupported_runtime`。后台程序内部调用必须使用结构化参数，不通过 shell 拼接来调用其他程序。

本地包导入由 WebUI 提交设备上的绝对路径。`bybusd` 以 root 读取该路径，将归档复制到模块私有目录并计算 SHA-256，然后校验格式、解包和发布。WebUI 不读取或传输包内容。前端不得把任意路径或 URL 拼入 shell 命令。[WebUI 接口](https://kernelsu.org/guide/module-webui.html)

## 4. 汉化源与数据准备

### 4.1 原文发现

`bybusd` 根据 Android 已安装包信息定位官方游戏的数据目录，再在确定的 `Localize` 根目录下只读枚举 `jp`、`en`、`kr` 文本。它不得扫描账号、存档或其他任意私有文件。目录下的路径必须规范化并验证仍位于允许根内。原文尚未下载时，状态为 `waiting_for_game_text`，已验证汉化归档继续保留。

现有 `GameStorage` 接口可作为功能清单；`ContainerWorkspaceStorage` 的容器路径不能直接用于原生游戏。新实现应把来源定位、文本枚举和原文读取放在单独适配层。[存储接口](../source/app/src/main/java/com/example/limbuszhcn/gamefs/GameStorage.kt) · [容器存储](../source/app/src/main/java/com/example/limbuszhcn/container/ContainerWorkspaceStorage.kt)

### 4.2 归档读取

默认源使用项目现有固定 GitHub Release 地址，选择名称严格匹配 `LimbusLocalize_<数字版本>.7z` 的资源。校验上游提供的 SHA-256；上游未提供时记录本地计算值用于缓存一致性，不将其视作发布者认证。本地源只接受兼容的 ZIP、7z JSON 文本包；保留路径遍历与重复目标校验，不对文件数量、展开大小或文本内容设置预设限制，并忽略包内插件及可执行内容。[默认源解析](../source/app/src/main/java/com/example/limbuszhcn/update/TranslationUpdateManager.kt) · [本地包边界](../source/app/src/main/java/com/example/limbuszhcn/translation/LocalTranslationArchive.kt)

### 4.3 离线配对

对于每张已下载的原文表，按现有 `TranslationJsonPairer.merge` 规则，使用表名、稳定记录 ID、显示字段、嵌套结构与重复 ID 消歧规则生成合并后的完整 JSON。英、韩、日资源各自按对应原文与同一份包内译文配对。未匹配字段保留原文；数字、游戏逻辑字段、资源元数据和数组结构不改变。`TranslationSnapshot` 的原文哈希绑定规则也必须纳入兼容性用例。[配对规则](../source/app/src/main/java/com/example/limbuszhcn/container/TranslationJsonPairer.kt) · [字段白名单](../source/app/src/main/java/com/example/limbuszhcn/container/TranslationTextPolicy.kt) · [快照绑定](../source/app/src/main/java/com/example/limbuszhcn/container/TranslationSnapshot.kt)

纯模块无法直接执行原有 Kotlin 类。首版用原生后台程序实现离线配对，并以原仓库的真实包及原文样本建立差分验证，逐字段比较原实现与新实现的输出。配对器属于高风险移植点；规则未覆盖的表要保持原文，不能通过按字符串全局替换补齐。

建议的原生接口为 `ArchiveImporter::import(path) -> VerifiedPackage`、`GameTextScanner::scan() -> SourceSet`、`JsonPairer::merge(resourceKey, original, translated) -> MergeResult` 和 `SnapshotBuilder::build(package, sources) -> Generation`。`MergeResult` 同时返回合并内容、匹配字段数、未匹配字段数及失败原因。归档读取器不承担 JSON 配对；配对器不读取磁盘；快照构建器负责在提交前复核所有输入的哈希。

### 4.4 版本化快照

每次成功构建在私有状态目录下生成新的不可变目录 `generations/<generation-id>/`，包含 `manifest.json`、各表合并 JSON、字体文件和必要的显示后备索引。清单至少记录 `schema`、所选来源 ID、归档 SHA-256、游戏版本与安装标识、每张表的规范资源名、原文 UTF-8 摘要、合并结果摘要和字节长度。生成 ID 基于内容摘要，不依赖时间戳判定有效性。

清单建议采用以下最小格式。资源名统一为 `Localize/<locale>/<path>/<LANG>_<table>.json`；当游戏只传表名时，构建器仅允许在全体已发布表中唯一定位。`mergedPath` 必须是 generation 目录内的相对路径。

```json
{
  "schema": 1,
  "generation": "sha256-of-canonical-inputs",
  "sourceId": "zero",
  "archiveSha256": "64 lowercase hex characters",
  "gameVersionCode": 0,
  "gameInstallIdentity": "sha256-of-installed-package-identity",
  "entries": [
    {
      "resource": "Localize/jp/JP_Example.json",
      "originalSha256": "64 lowercase hex characters",
      "mergedPath": "tables/sha256-of-merged.json",
      "mergedSha256": "64 lowercase hex characters",
      "mergedBytes": 0
    }
  ]
}
```

上述字符串是格式占位符，构建器必须写入实际摘要和字节数。WebUI 和游戏进程均只消费经过 schema、路径、文件长度和摘要验证的清单。协议或配对规则变化时提升 schema，旧 generation 保留到新版本成功发布。

构建先写临时目录并逐项校验，完成后刷新数据与目录元数据，再原子切换 `active` 指针。中断、解包失败或源文件在构建期间变化时，继续使用上一完整快照。游戏进程启动时固定使用一个 generation；WebUI 切源和后台更新不改变正在运行的进程。保留有限数量的旧 generation，确认没有运行进程引用后再清理。

原文摘要以游戏加载器返回给 Hook 的 UTF-16 文本转换为 UTF-8 后计算。构建时若只拿到磁盘字节，需验证磁盘解码结果与游戏实际返回文本一致。BOM、换行或加载器预处理造成的差异应导致该表回退原文并留下诊断，不允许放宽为仅按文件名替换。

## 5. 游戏进程运行时

Zygisk 模块在进程 specialization 阶段识别官方包名、进程名和 UID。只在 `com.ProjectMoon.LimbusCompany` 主进程准备运行时。进程已进入应用沙箱时，通过 Zygisk companion 获取只读的快照句柄；模块不依赖直接读取 BYBUS 私有目录，也不在游戏进程执行下载和解包。[Zygisk API](https://github.com/topjohnwu/zygisk-module-sample/blob/master/module/jni/zygisk.hpp)

运行时须从现有 `LimbusTranslationRuntime.cpp` 提取 IL2CPP 方法发现、文本读取、字体与必要的显示后备逻辑。移除 `VirtualCore`、`NativeEngine`、虚拟包服务、容器目录重定向和容器内的 Java 回调依赖。新运行时在原读取函数返回后取得资源名及原始 JSON，查找 `(资源名, 原文摘要)` 完全匹配的快照条目，核对快照条目长度与摘要后创建新的 IL2CPP 字符串。任何环节失败都返回原始对象。`open`、`openat` 等游戏资源文件读取不进入本方案的汉化路径。[现有读取 Hook](../source/third_party/virtualapp-upstream/lib/src/main/jni/Foundation/LimbusTranslationRuntime.cpp) · [现有容器回调](../source/third_party/virtualapp-upstream/lib/src/main/java/com/lody/virtual/client/NativeEngine.java)

进程内接口可收敛为 `SnapshotReader::open(readOnlyFd)`、`SnapshotReader::lookup(resourceKey, originalSha256)`、`TranslationRuntime::onTextRead(resourceKey, originalIl2cppString)` 和 `FontRuntime::ensureReady()`。`onTextRead` 调用原方法取得结果后才执行查找。句柄交付失败或查找未命中时不创建新字符串。运行时按 `generation`、命中数、未命中数和错误码记录计数，不保存正文。

Zygisk 入口要保持轻量；现有原生代码广泛使用 C++ 标准库，不能未经隔离就把整个 VirtualApp 原生库直接注入 Zygote。构建时应评估自包含 C++ 运行时与宿主库冲突，并在空模块阶段先验证加载和卸载。官方 Zygisk 样例对其 C++ STL 配置有专门说明。[Zygisk 样例](https://github.com/topjohnwu/zygisk-module-sample)

中文字体沿用项目现有授权字体，并在模块安装或快照构建时按固定大小及 SHA-256 校验。Unity 当前的字体加载入口要求可读取的文件路径，因此快照句柄本身不能作为字体方案。首选在实机验证已打开字体描述符的 `/proc/self/fd/<n>` 路径是否可被 Unity 接受；若不支持，由后台服务在游戏私有数据的独立辅助目录放置经校验的字体副本，并配置游戏 UID 可读取的权限与上下文。该辅助目录不位于 `Localize` 或 Unity 下载资源目录。运行时确认字体加载成功后才返回中文 JSON，不能沿用原代码中从容器 `active-index.path` 反推出字体路径的假设。[原字体校验](../source/third_party/virtualapp-upstream/lib/src/main/java/com/lody/virtual/helper/compat/LimbusTranslationAssets.java) · [原字体路径](../source/third_party/virtualapp-upstream/lib/src/main/jni/Foundation/LimbusTranslationRuntime.cpp)

## 6. 兼容性与验证门槛

原版在 Root 环境可玩是本项目的设备基线。Zygisk 注入、IL2CPP Hook 和字体加载会形成新的运行条件。现有针对 VirtualApp 的 AppSealing 处理、版本配置及保护路径不作为移植完成的证据，也不能直接搬入新运行时。每个受支持游戏版本都要分别记录：原版基线、仅加载空 Zygisk 模块、启用文本 Hook、应用快照后的启动与安全提示结果。出现安全提示或退出时停止宣称兼容，并保留最少必要诊断。[原项目安全诊断](../source/docs/startup-safety-diagnostics.md) · [版本约束](../source/third_party/virtualapp-upstream/lib/src/main/java/com/lody/virtual/helper/compat/LimbusNativeProfile.java)

验收按以下顺序执行。

1. WebUI 能识别模块、Zygisk 依赖、游戏安装状态，并显示固定错误码。
2. 官方及本地归档通过校验后，可针对已有原文生成快照；同一输入重复构建得到相同内容。
3. 在桌面样本上，新配对结果与原 Kotlin 配对器逐字段一致。

    AppSealing 兼容注入的本地计划与运行时装在 `12-kernelsu-appsealing.md` 描述（`profile.appsealing` 计划、detach 兼容点、报告桥与站点补丁、失败回滚）；它只是实现说明，不作为实机通过证据。
4. 空 Zygisk 模块下官方游戏到达标题页，再分别验证文本 Hook、字体和一张 UI 表的中文显示。
5. 连续启动、切换来源、包更新、原文更新和构建中断均保留完整快照；新原文重新构建后在下一次游戏启动生效。
6. 运行前后官方语言文件及下载清单摘要一致，游戏没有因本模块的汉化路径反复下载相同资源。

## 7. 开发顺序与剩余未知

先完成静态 WebUI、控制程序协议及原生归档/配对器。随后验证只读原文发现、快照原子发布及字体准备。最后接入只针对官方游戏主进程的 Zygisk 运行时。首个实机里程碑是“空注入仍可游玩”，第二个是“单张已下载表显示中文”，第三个才是完整覆盖及来源切换。

实际游戏原文目录、加载器是否改变 JSON 字节表示、root 守护进程读取本地绝对路径的能力，以及注入后的游戏保护反馈，均需要设备实测。文档不把源码推断写成实测结论。
