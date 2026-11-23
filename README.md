# LimCN4KSU

BYBUS 的 KernelSU 原生移植：在官方 `com.ProjectMoon.LimbusCompany` 安装包上，通过 Zygisk 模块完成本地化快照读取、内存文本替换、字体准备与 AppSealing 兼容注入。

## 结构

| 目录 | 说明 |
| --- | --- |
| `src/shared` | JSON、SHA-256、UTF 编解码、协议码、文件工具 |
| `src/translation` | 归档导入、JSON 配对、文本策略、快照构建、索引编译、AppSealing 计划 |
| `src/service` | `bybusd` 后台任务、来源、代次、导入、诊断 |
| `src/control` | `bybusctl` 控制入口 |
| `src/runtime` | Zygisk 模块、IL2CPP Hook、字体、索引路由、AppSealing 注入 |
| `webui` | Vite 构建的 KernelSU WebUI 源 |
| `module` | 模块安装包骨架（WebUI、脚本、字体、Zygisk/可执行文件由构建生成） |
| `tests` | 主机纯数据测试 |
| `third_party` | LZMA SDK、Zygisk API、Substrate |

## 构建

需要 Android NDK r30、Node.js ≥ 20、PowerShell 5.1。

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
4. 打开 WebUI，检查游戏、Zygisk、快照与 `profileState`。
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

- 纯数据层、arm64 交叉编译、模块打包与主机测试已通过。
- L0–L3 实机矩阵仍为未验证；AppSealing 的实际检查时机与游戏反馈需要按版本记录。
- AI 生成、VirtualApp 容器、游戏安装/更新、自更新与计费不在本模块范围。

## 许可

第三方组件许可见 `THIRD_PARTY-NOTICES.md` 与各 `VENDOR-RECORD.txt`。
