# 第三方组件与来源声明

版本：2026-11-23。本目录中的第三方代码只用于构建 `bybusd`、`bybusctl` 与 `zygisk/arm64-v8a.so`，不改变其原始许可。

## 1. 7-Zip LZMA SDK（7z / LZMA / LZMA2 / BCJ / PPMd 解码）

- 位置：`third_party/lzma-sdk/`
- 来源：`https://github.com/ip7z/7zip` 的 `C/` 目录（默认分支），逐文件抓取，清单见 `third_party/lzma-sdk/VENDOR-RECORD.txt`
- 许可：**public domain**（7-Zip LZMA SDK 声明）
- 用途：只读解析本地与默认源的 `.7z` 文本包；不参与编码，也不写文件
- 本地修改：无。构建脚本以 `-w` 编译该目录，避免上游告警触发本项目的 `-Werror`
- 未包含：`Ppmd7*.c` 之外的编码器、AES、Zstd、Xz 相关文件

## 2. Zygisk API 头文件
- 位置：`third_party/zygisk/jni/zygisk.hpp`
- 来源：`https://github.com/topjohnwu/zygisk-module-sample`（`module/jni/zygisk.hpp`）
- 许可：文件内自带的 ISC 风格许可（"Permission to use, copy, modify, and/or distribute this software for any purpose with or without fee is hereby granted"）
- 用途：`src/runtime/zygisk_module.cpp` 的模块/companion 入口声明；未修改该头文件
- 注意：API 版本 5。设备上的 Zygisk 实现（ZygiskNext）必须提供与之兼容的 API

## 2.1 kernelsu WebUI 库（打包进 `webroot/app.js`）

- 位置：`module/webroot/app.js`（Vite 打包内联）；依赖声明在 `webui/package.json`
- 来源：npm `kernelsu@3.3.0`，上游 `https://github.com/tiann/KernelSU`
- 许可：**Apache-2.0**，随模块附带 `module/webroot/NOTICE.txt`
- 用途：模块 WebUI 与 KernelSU 管理器交互（`exec` 等）；库逻辑未修改，仅经 Vite 压缩
- 构建：`cd webui && pnpm install --config.strictDepBuilds=false && pnpm build`

## 2.2 Vite / esbuild（仅构建期，不进入模块）

- 位置：`webui/node_modules/`（打包脚本不读取该目录）
- 许可：Vite MIT、esbuild MIT
- 用途：把 `kernelsu` 与页面脚本合成单个经典脚本；运行时无依赖

## 3. Substrate / And64InlineHook（arm64 内联 Hook）

- 位置：`third_party/substrate/`
- 来源：从 BYBUS 仓库 `third_party/virtualapp-upstream/lib/src/main/jni/Substrate/` 复制（提交 `bd32608`），另加同仓库 `Foundation/RuntimePage.{h,cpp}`、`Foundation/OwnedExecutablePages.h`
- 许可：Cydia Substrate 的 BSD 声明与 `And64InlineHook` 的 MIT 声明保留在各自源文件中
- 本地修改：
  - 三处 `#include "../Foundation/RuntimePage.h"` 改为 `#include "RuntimePage.h"`（文件已同目录复制）
  - 未改动任何函数体
- 用途：在 `libil2cpp.so` 的本地化读取方法上安装最小补丁；补丁字节全部记入 `src/runtime/integrity.cpp` 的补丁账本

## 4. 中文字体

- 位置：`module/assets/ChineseFont.ttf`、`module/assets/font.sha256`、`module/assets/Sarasa-Gothic-OFL-1.1.txt`
- 来源：BYBUS 仓库 `app/src/main/assets/runtime/ChineseFont.ttf`（上游为 LocalizeLimbusCompany 的 `Fonts/ChineseFont.ttf`），字体面 `Sarasa-Gothic-SC-Bold`
- 许可：**SIL Open Font License 1.1**，全文随模块分发
- SHA-256：见 `module/assets/font.sha256`（`bybusd` 构建快照时校验，运行时不匹配即拒绝）

## 5. 从 BYBUS 移植的行为逻辑

以下为本项目重写的 C++ 实现，语义基线是 BYBUS 的同名 Kotlin/C++ 源（提交 `bd32608`）：

| 本实现 | 基线 |
| --- | --- |
| `src/translation/json_pairer.cpp` | `app/src/main/java/com/example/limbuszhcn/container/TranslationJsonPairer.kt`、`TranslationSnapshot.kt` |
| `src/translation/text_policy.cpp` | `.../container/TranslationTextPolicy.kt` |
| `src/translation/archive_import.cpp` | `.../translation/LocalTranslationArchive.kt` |
| `src/runtime/hooks.cpp`（类与方法发现、读后合并） | `third_party/virtualapp-upstream/lib/src/main/jni/Foundation/LimbusTranslationRuntime.cpp` |
| `src/runtime/integrity.cpp`（补丁账本、CRC 归一化） | `.../Foundation/LimbusCodeIntegrityCompat.cpp`、`LimbusIntegrityLayout.h` |
| `src/runtime/appsealing.cpp`、`src/runtime/appsealing.h`（AppSealing 兼容注入） | `.../Foundation/LimbusCodeIntegrityCompat.cpp`（报告桥与站点补丁）、`LimbusReportBridge.h` |
| `src/runtime/owned_code.h`（自有代码段范围） | `.../Foundation/LimbusOwnedCode.h` |
| `src/translation/appsealing_profile.cpp`、`src/translation/appsealing_profile.h`（计划资格判定） | `.../Foundation/IORelocator64.cpp` 的 AppSealing 部分 |

上表的 AppSealing 三项以 BYBUS 的 `third_party/virtualapp-upstream/lib/src/main/jni/Foundation/` 为基线（提交 `bd32608`），只保留结构资格判定、报告桥、站点补丁与自有代码段 CRC 的表达方式；所有偏移、期望指令字、栈布局与站点常量改由 `bybusd` 从本机 APK 生成的 `profile.appsealing` 提供，代码中不写死版本常量。

按 `02-code-quality-standard.md` §1，移植保留了原始路径与修改说明；逻辑本身不复制 VirtualApp 的容器重定向、JNI 回调与版本常量。具体地，未复制 `IORelocator` 的容器文件描述符重定向、`MapsRedirector` 的映射改写与 `syscall` Hook：本模块的运行时只在官方游戏主进程内工作，不改变文件路径解析语义。

## 6. 未打包的依赖

`ZygiskNext` 不随本模块分发、不安装、不修改；模块只在 `status` 中报告其是否存在。
