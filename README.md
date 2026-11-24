# HP OMEN 8 macOS 移植与修复

本仓库记录如何从 Windows 用 **NullMoth 1401** 构建初始 OpenCore EFI，再逐步完成 HP OMEN 8 的图形、输入、音频和固件控制适配。实机为 HP OMEN 16-b1xxx（暗影精灵 8）、Core i5-12500H、Iris Xe、RTX 3060 Laptop、1920×1080 内屏，主要验证基线为 **macOS Tahoe 26.7.3（25G342）x86_64**。其他 BIOS、子型号和 macOS 构建需重新验收。 [Apple 软件许可](https://www.apple.com/legal/sla/)

## 从 Windows 开始

1. **构建 1401 初始 EFI。** 在 Windows 用 1401 检测硬件、生成 OpenCore 配置并写入恢复 U 盘。先备份 Windows EFI 和原始 U 盘配置，另留一份可启动的回退盘。
2. **安装并建立可启动基线。** 从明确的 OpenCore 入口启动，核对恢复联网、目标磁盘、键盘和 USB。按本机硬件补齐 I2C、USB 映射、Wi-Fi 和蓝牙。
3. **逐项部署修复。** 先验证 Xe 内屏和加速，再验 RTX 3060 直连。先确认扬声器，再加 DMIC 与 HAL，最后验证 OMEN MUX。每次更新一个组件族，记录版本和实测结果。

[完整安装教程](docs/install-from-windows.md) · [1401 Windows 发行包](https://github.com/nullmoth/1401/releases)

## 修复范围和维护入口

| 组件 | 来源与本机改动 | 升级时重点核对 |
| --- | --- | --- |
| OpenCore、UEFI 与 ACPI | NullMoth 1401 基线，使用 OpenCorePkg 引导。恢复后核对了 picker、NVRAM、设备属性、UEFI 工具和十张 SSDT。 | 同版更新 BOOTx64、OpenCore、OpenRuntime、OpenCanopy 与资源，使用同版 ocvalidate。复查 BIOS 变动后的 ACPI 路径。 [启动链](docs/engineering.md#2-opencoreuefi-和启动配置) · [ACPI](docs/engineering.md#6-acpi-表和重命名) |
| Iris Xe 内屏 | 基于 b00t0x 的 WhateverXe 0.8.2。HP 面板适配涉及 `kern_vbt.hpp`、`kern_firmware.cpp` 和 `kern_xe.cpp`，针对本机 VBT 和受保护的 DDI0 接管路径。已确认 Xe 驱动 1080p 内屏并提供 Metal 2。 | 在原作者新版源码逐处移植或复核三个改动点。配套检查 WhateverXe Graphics、签名信任链及 AuxKC。 [移植手册](docs/engineering.md#3-图形栈与-xe-补丁移植) · [Xe 补丁](patches/WhateverXe-0.8.2-HP-Omen.patch) |
| RTX 3060 | NullMoth 3.0 的 NVRM、NVRMFB、NVRMAGDC、NVAccel、NMXAudio。曾实测独显直连驱动内屏。 | 按上游安装清单同步系统组件与 EFI 参数。分别验证混合、直连、内外屏与睡眠。早期偶发的黑屏仍缺故障现场证据。 [3060 与图形依赖](docs/engineering.md#系统图形组件信任和-rtx-3060) |
| 扬声器与内建麦克风 | AppleALC 为 ALC245 提供模拟输出。SimpleHDA 1.2.0 将 AppleHDADmicBridge 嵌入 AppleHDA 路线，配合 SOF IPC4 和独立 HPIPC4Audio HAL。已确认输入电平、QuickTime 回放与 Siri。 | 在新版 SimpleHDA 原作者包内复核六处集成改动，并让内核桥、固件/拓扑、DMA 生命周期与 HAL ABI 配套。实测反复开关录音。 [音频维护](docs/engineering.md#4-音频与麦克风) · [集成补丁](patches/SimpleHDA-OMEN-DMIC.patch) |
| 触控板、USB 与联网 | VoodooI2C 加本机 51E8 注入，USBToolBox 加本机端口图，Intel AX211 与蓝牙使用配套驱动。 | 检查驱动父子依赖、单一 VoodooInput、USB 映射及目标 macOS 版本。分别测试睡醒后的输入和连接。 [外围驱动维护](docs/engineering.md#5-输入usb网络蓝牙和基础-kext) |
| OMEN MUX | 应用 0.2.6、驱动 0.1.9。通过 HP WMI 读写混合/直连模式、性能档和风扇，并展示温度与负载。 | 新 BIOS 和机型先用只读变体验证方法与读回值。更新请求或 IORegistry 属性时同步 app 与 kext；切换后重启验证面板归属。 [MUX 实现与维护](docs/engineering.md#7-omen-mux-固件控制项目) |

[逐组件维修与升级手册](docs/engineering.md)覆盖当前 EFI 的 55 个配置项，注明来源、版本、是否有本机源码差异、升级文件与验收办法。

## 下载与构建

- **发布附件**包括 OMEN MUX 应用和驱动、HPIPC4Audio HAL，以及 SHA-256 校验清单。HAL 需与 SimpleHDA/AppleHDADmicBridge 配套。 [下载 v0.1.1](https://github.com/sourire0/omen8-macos/releases/tag/v0.1.1)
- **源码与补丁**包括 [OMEN MUX](src/omen-mux)、[DMIC 内核桥与 HAL](src/dmic-bridge)及 [WhateverXe 和 SimpleHDA 补丁](patches)。AppleHDA 原版载荷、SOF 固件及其他外部依赖按构建说明准备。 [构建与部署](docs/build.md)
- **EFI 参考**包含 [配置片段](efi/OMEN8-reference.plist)、[51E8 注入](efi/OmenI2C-51E8.kext/Contents/Info.plist)及 [USB 映射](efi/UTBMap.kext/Contents/Info.plist)。配置片段需合并到同版 OpenCore 配置、填入自己的机器标识并完成校验后使用。
