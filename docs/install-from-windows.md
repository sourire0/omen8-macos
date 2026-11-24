# 从 Windows 制作启动盘并验证 macOS

## 1. 确认硬件和恢复条件

在 Windows 中用设备管理器的「详细信息 → 硬件 ID」记录显卡、网卡、音频控制器和 I2C 控制器。用 `msinfo32` 确认 UEFI 模式，记录 BIOS 版本、存储控制器模式和显卡模式。用 Windows 自己的录音工具确认实体内建麦克风可用。公开求助时只附通用 PCI ID 和必要的子系统 ID。

备份文件和 Windows EFI，保存 BitLocker 恢复密钥并准备 Windows 恢复介质。优先使用独立 SSD 安装 macOS；双系统可在 Windows 先压缩目标卷，保持 Windows EFI、恢复分区和数据分区原样。准备两个 U 盘，一个用于测试，一个保留经过启动验证的 EFI。固件优先选混合显卡模式。存储依赖 VMD/RST 的机器先弄清 Windows 的切换及恢复过程，贸然改 AHCI 可能导致 Windows 无法启动。

## 2. 在 Windows 准备 UEFI 安装盘

1. 下载同一发行版本的 OpenCorePkg、Python 3 和 plist 编辑工具。准备支持该 macOS 版本的有线网络，在线恢复需要能访问 Apple 服务器。
2. 安装 Python 时确认 Python Launcher 可用。在 PowerShell 进入 OpenCorePkg 的 `Utilities/macrecovery`，用 `py -3 macrecovery.py -h` 检查参数，再按该版本的 Windows 指南使用目标恢复版本对应的 board ID。命令形态为 `py -3 macrecovery.py -b <BOARD_ID> -m <MODEL_VALUE> download`，将占位符换成指南值。目录内应同时有恢复镜像和对应的 chunklist。
3. 在磁盘管理里凭容量和插拔确认 U 盘。备份 U 盘内容后，将目标 U 盘准备为可供 UEFI 读取的 FAT32 分区。将恢复文件放入根目录的 `com.apple.recovery.boot`，将 `EFI` 文件夹并列放置。用 `Get-ChildItem X:\` 核对盘符，再检查 `X:\EFI\BOOT\BOOTx64.efi`、`X:\EFI\OC\OpenCore.efi` 和 `X:\EFI\OC\config.plist`。
4. 可以使用目标版本支持的 1401 Windows 构建器生成初始 U 盘，然后检查上述布局及 EFI 内容；图形和音频仍须按下文逐步验证。历史安装脚本可能要求自动登录或账户凭据，审阅脚本并依照当前上游说明执行。

[OpenCorePkg](https://github.com/acidanthera/OpenCorePkg) · [Dortania Windows 制作指南](https://dortania.github.io/OpenCore-Install-Guide/installer-guide/windows-install.html) · [1401](https://github.com/nullmoth/1401)

## 3. 组装基本 EFI

从同版 Sample.plist 和 X64 EFI 起步。使用本仓库 EFI 片段对照各个条目，根据自己的 ACPI 和 PCI 路径选择，保留 Sample.plist 当前版本的字段及类型。不要直接将片段保存为活动 config.plist。

- ACPI 从自己的固件表出发。分别检查 EC、USBX、PLUG、RTC/AWAC、GPIO、PNLF 和 XOSI 是否需要、设备路径是否存在。表名相同也可能适配不同固件。
- 先启用 Lilu、VirtualSMC、网络及内建键盘所需驱动，再补 I2C 输入、USB 映射和电池传感器。USBToolBox 可在 Windows 采集映射，确认 USB 2/3 和内建蓝牙端口后生成 UTBMap；新系统维持 `XhciPortLimit=false`。
- 选择合适的 SMBIOS 机型。实机参考为 MacBookPro16,2。给自己的机器生成独立序列、MLB、SystemUUID 和 ROM，保存在本地。暂缓登录 Apple 账户，先确保基础硬件和启动稳定。
- 对照依赖次序和 `Kernel → Add` 的 BundlePath、ExecutablePath、PlistPath、Enabled。含子插件的路径须完整。不要双重注入 VoodooInput。UEFI 驱动和 OpenCore 版本须配套。
- 运行同版 `ocvalidate.exe X:\EFI\OC\config.plist`。备份当前 EFI，记录 config 的 SHA-256 和当前版本，才开始试机。日志只在明确需要时启用，输出留在本机。

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
