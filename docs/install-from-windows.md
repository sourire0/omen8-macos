# 从 Windows 制作启动盘并验证 macOS

## 1. 确认硬件和恢复条件

在 Windows 中用设备管理器的「详细信息 → 硬件 ID」记录显卡、网卡、音频控制器和 I2C 控制器。用 `msinfo32` 确认 UEFI 模式，记录 BIOS 版本、存储控制器模式和显卡模式。用 Windows 自己的录音工具确认实体内建麦克风可用。公开求助时只附通用 PCI ID 和必要的子系统 ID。

备份文件和 Windows EFI，保存 BitLocker 恢复密钥并准备 Windows 恢复介质。优先使用独立 SSD 安装 macOS；双系统可在 Windows 先压缩目标卷，保持 Windows EFI、恢复分区和数据分区原样。准备两个 U 盘，一个用于测试，一个保留经过启动验证的 EFI。固件优先选混合显卡模式。存储依赖 VMD/RST 的机器先弄清 Windows 的切换及恢复过程，贸然改 AHCI 可能导致 Windows 无法启动。

## 2. 在 Windows 用 1401 构建初始 EFI 和安装盘

用 [NullMoth 1401 Windows 发行包](https://github.com/nullmoth/1401/releases) 构建，记录发行版、所选的 macOS 版本、硬件报告和工具触发的规则。1401 以 OpCore-Simplify 为生成基础。本手册的最终实测是 Tahoe 26.7.3。1401 的默认选择、可选版本和 NVIDIA 安装规则会随发行版变化，先确认当前发行支持所选组合，再制作安装盘。保留 Windows EFI 和单独的备用盘。

1. 在 Windows 完整解压 `1401-Windows-<版本>.zip`，保留程序和配套目录。以管理员身份运行 `1401.exe`，允许它读取本机硬件和写入目标 U 盘。
2. 选择 **Check this PC**。核对 i5-12500H、Iris Xe、RTX 3060、AX211、存储和网卡是否正确识别。用 **Build the Mac setup** 生成。阅读 **Your BIOS steps** 并记录所建议的设置。首次安装优先验证混合显卡模式和恢复环境的联网能力。
3. 选择 **Create the macOS stick**。使用可擦除的 4 GB 或更大 U 盘，按容量、型号和盘符确认目标。在线恢复需要互联网及兼容网卡。等待 1401 写盘结束并重新识别盘符。
4. 在 PowerShell 用 `Get-ChildItem -LiteralPath 'X:\'` 查看 U 盘。核对 `EFI\BOOT\BOOTx64.efi`、`EFI\OC\OpenCore.efi`、`EFI\OC\config.plist`，以及 `com.apple.recovery.boot` 下配对的恢复镜像与 chunklist。若生成了 `NullMoth` 目录，随原始输出保留。
5. 在 Windows 本地备份未经修改的整个 `EFI`，保留 1401 的版本、硬件报告和构建结果。用 `Get-FileHash X:\EFI\OC\config.plist -Algorithm SHA256` 记录原始配置指纹。日志与硬件报告可能包含设备或用户信息，上传前先审阅。

[1401 使用说明](https://github.com/nullmoth/1401#using-it) · [1401 构建原理](https://github.com/nullmoth/1401/blob/main/docs/HOW-IT-WORKS.md)

1401 无法生成合适配置时，可用同版 OpenCorePkg 里的 `Utilities/macrecovery` 和 Python 3，按指南下载目标版本的恢复镜像，自行组装 FAT32 U 盘。恢复镜像与 OpenCore 的 `EFI` 并列放在 U 盘根目录。 [Dortania Windows 制作指南](https://dortania.github.io/OpenCore-Install-Guide/installer-guide/windows-install.html)

## 3. 从 1401 初始 EFI 补到本机基线

先按下一节进入恢复或桌面并记录启动结果；若恢复阶段就有键盘或网络问题，只先补对应组件。把原始 EFI 留作差异基准，工作副本按以下顺序改。初始构建已有的驱动、ACPI 和配置都需要按实际生成结果检查。

| 顺序 | 针对这台 OMEN 8 的变更 | 测试停点 |
| --- | --- | --- |
| 启动 | 对照 `EFI/BOOT`、`EFI/OC` 和恢复目录。需要迁移时先校验 OpenCore 与驱动版本、Vault、ScanPolicy、UEFI 驱动、SMBIOS、安装/回退入口。 | 原始启动路径和恢复入口可用，记住该测试盘。 |
| 输入、USB、网络 | 按硬件匹配选择 VoodooI2C 系列和 OmenI2C-51E8，保留单一 VoodooInput。生成 UTBMap 后核对 USB，按系统版本补 AirportItlwm、蓝牙配套及有线网。 | 键盘、触控板点按、USB、恢复联网、蓝牙配对逐项通过。 |
| 图形 | 先配同代 WhateverXe 与 Graphics，迁移三处 HP 源码补丁；再按 NullMoth 发行说明部署 3060。 | Xe 内屏与加速正常后再验独显直连。 |
| 声音和固件控制 | 先测 AppleALC + SimpleHDA 扬声器，再部署带 DMIC helper 的 SimpleHDA、配套 HAL 和 OMEN MUX 驱动/应用。 | 录音复开、Siri、图形模式写后读回和重启后面板归属逐项验收。 |

- 逐项比较本仓库 [EFI 参考片段](../efi/OMEN8-reference.plist) 与生成的完整 plist。保留生成器对应 OpenCore schema，按本机 ACPI/PCI 路径选择，使用自己的 SMBIOS 值。参考片段必须合并才能使用。
- 在 Windows 先运行同版 `ocvalidate.exe X:\EFI\OC\config.plist`。逐项核对 `Kernel → Add` 的 BundlePath、ExecutablePath、PlistPath、Enabled、插件嵌套路径及依赖次序。换盘或重建时重新核对目标盘符。
- 详细版本、来源、定制文件和升级落点见 [逐组件维护手册](engineering.md)。若安装后运行 1401.app，它可能修改 U 盘配置，保留安装前后副本，避免覆盖已验证的本机补丁。

[OpenCore 配置文档](https://github.com/acidanthera/OpenCorePkg/tree/master/Docs) · [USBToolBox](https://github.com/USBToolBox/tool)

## 4. 安装并建立回退基线

从 BIOS 的一次性启动菜单明确选测试 U 盘的 UEFI 项，进入 OpenCore 后选恢复环境。用磁盘工具「显示所有设备」核对目标型号、容量和分区。独立目标 SSD 可用 GUID 分区表和 APFS；同盘双系统只操作预先确认的 macOS 目标空间。联网安装期间每次回到同一个 OpenCore，并选择内置盘的 macOS Installer，直至出现系统初始设置。遇到恢复镜像找不到，先核对目录、ScanPolicy 和 USB；遇到内核报错，记录末屏和时间后返回已知可启动配置。

到达桌面后，确认本次启动盘、网络、键盘、USB 和分辨率；保存工作 EFI 与对应系统组件版本。确认回退 U 盘真能启动后，再开始升级。仅在所有验收通过后考虑把已验证 EFI 复制到系统盘自己的 EFI 分区，并保留 Windows 的 Microsoft 引导目录。

## 5. 完成图形、外围和音频

1. **Xe 核显**。为实机采用同代 WhateverXe kext 和 Tahoe Graphics 系统包，按照上游方案处理签名信任与 AuxKC；先审查安装包。应用仓库中的 0.8.2 面板补丁前核对 PCI、子系统和 VBT 条件。EFI 用 Lilu 加 WhateverXe，避免与 WhateverGreen 并行。重启后通过 `system_profiler SPDisplaysDataType` 检查内屏归属、分辨率、加速，实测亮度、播放、外接屏和唤醒。
2. **RTX 3060**。从 NullMoth 上游取得对应系统版本组件。先留好 Xe 可启动档，再装驱动并用 `kmutil showloaded` 检查 NVRM、NVRMFB、NVRMAGDC、NVAccel。先用固件选项验证独显直连，确认内屏由 RTX 驱动。黑屏只剩指针时记下精确时间，确认指针能否移动和重启方式，再切回 Xe 查 WindowServer 与驱动日志。历史测试变量 `-nvrmnoflip` 仅用于对照，详见工程记录。
3. **外围**。按各自版本矩阵加入 Wi-Fi、蓝牙、有线网、I2C/PS2、电池与亮度键。I2C 8086:51E8 的本机注入可参考 EFI 目录，确认设备匹配及插件加载。冷热启动、休眠后分别试网络、触控板、各 USB 端口、电池显示和亮度；AirDrop/AWDL 单独验收。
4. **音频**。先使用 AppleALC、匹配 layout（本机 HDEF 为 13）和 SimpleHDA 建立 ALC245 的模拟扬声器基线。再按构建文档将 DMIC helper 嵌到 SimpleHDA，并安装配套 HPIPC4Audio HAL。只让 AppleHDA 持有 PCI 控制器，旧 HPIPC4 PCI 和共享探针保持关闭。移走重复的试验 HAL，检查活动驱动与 HAL 装载状态。
5. **OMEN MUX**。先用只读变体确认驱动服务、ACPI 方法、风扇数量及读值。匹配本机固件并保留启动回退后，再装切换变体。图形模式写入固件后需重启生效；性能档和风扇即时生效。重启后确认显示器实际挂在预期 GPU。

[WhateverXe](https://github.com/b00t0x/WhateverXe) · [NullMoth NVIDIA](https://github.com/nullmoth/nvidia-macos-driver) · [AppleALC](https://github.com/acidanthera/AppleALC)

## 6. 验收与维护

按固定顺序做冷启动、待机后复测、混合和独显两次启动；观察扬声器和内屏，随后讲话看输入电平，用 QuickTime 录音回放，反复关闭与新建录音，再单独唤起 Siri，间隔几分钟重新录音。若看到输入设备名但无信号，先确认实际 HAL、采集时钟和样本，不要只调输入音量。

升级时同时记录系统构建、OpenCore、kext 和图形系统包。每次只替换一个组件族，比较 `kmutil showloaded`、`ioreg` 与 `system_profiler`，记录是否回归。EFI 回退通常只恢复启动层；系统卷组件和 HAL 需要依据安装清单分别恢复。公开日志前去掉用户路径、网络信息、序列号和机器唯一 ID。
