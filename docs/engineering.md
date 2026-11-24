# HP OMEN 8 macOS 修复与逐组件升级手册

适用基线为 HP OMEN 16-b1xxx（暗影精灵 8）、i5-12500H、Iris Xe `8086:46A6`、HP 子系统 `103C:8A13`、RTX 3060 Laptop `10DE:2520`、1920×1080 内屏、macOS Tahoe 26.7.3（25G342）。版本和启用状态来自当前 1401 盘的只读盘点。系统侧结论另据运行态检查和历史交接。插着 U 盘不能证明由它启动，`Enabled=true` 也只代表 OpenCore 准关注入。本文的「对照基线」指 Windows 重建 1401 后保留的初始 EFI 副本。

- [从 Windows 用 1401 构建初始 EFI](install-from-windows.md)
- [自定义项目构建与部署](build.md)
- [不含机器标识的 EFI 配置片段](../efi/OMEN8-reference.plist)

## 1. 修复路线与现有结果

| 阶段 | 已确认结果与工程决策 |
| --- | --- |
| Windows 硬件调查 | 用 Windows SST 录音端点、NHLT 和设备实例确认 Intel cAVS `8086:51C8` 下有 `8086:AE20` PDM 阵列，双声道 48 kHz。Realtek ALC245 `10EC:0245` 是模拟 HDA 路径。触控板为 HID-over-I2C `373B:1012`，上游控制器 `8086:51E8`。 |
| EFI 基线 | 曾遇到 MULL 卡死、进不了系统，以及 1401 某些实验更改后进不了系统。旧记录里启动来源有过混淆。恢复后实验集中到 Windows 重建的 1401，MULL 用作离线备用。已有记录不足以给每次旧启动失败归因。 |
| Xe 核显 | 早期从低显存无加速推进至 Metal 2。最终用 WhateverXe 0.8.2 本机面板和端口补丁配套系统图形组件，运行态可见 Xe 驱动 1080p 内屏。亮度、外屏和睡眠仍应按版本分别复测。 |
| RTX 3060 | NullMoth 3.0 驱动已加载，核显启动时也能列出 RTX Metal 4。OMEN MUX 交接另记录了独显直连驱动内屏的实测。历史上进度条后黑屏有鼠标，并且故障时段缺该次直连启动日志，根因待定位。 |
| 扬声器与 DMIC | AppleALC + SimpleHDA 提供模拟输出。独立 HPIPC4 PCI 路线出现控制器争用，后改为 AppleHDA 单一所有权、嵌入子服务和配套 HAL。SimpleHDA 1.2.0 后用户确认电平、QuickTime 和 Siri 恢复。长期稳定性、跨系统升级另验。 |
| OMEN MUX | 应用 0.2.6、EFI 驱动 0.1.9。交接已实测混合/直连切换、性能三档、风扇全速和恢复自动、CPU/GPU/温度/双风扇监控。低温档与 Windows ECO 的功耗、风扇曲线是否相同尚未测量。 |

## 2. OpenCore、UEFI 和启动配置

以下路径以 `EFI/OC` 为根，另有 `EFI/BOOT/BOOTx64.efi`。当前 `OpenCore.efi` 和 `OpenCanopy.efi` 与初始副本字节不同，`BOOTx64.efi`、OpenRuntime、HfsPlus、apfs_aligned、ResetNvramEntry 一致。历史资料没有给出这两份改动二进制的源码提交，升级时从明确版本重新组套并验启动。

| 组件与来源 | 本机改动或用途 | 后续更新落点与验收 |
| --- | --- | --- |
| 1401，NullMoth，底层 OpCore-Simplify | Windows 端生成硬件基线、OpenCore 设置与恢复盘。之后在测试盘补本机修复。 | 更新 Windows 生成器先在空白测试 U 盘重建并留存构建结果，再比较旧配置、表和驱动。Mac 应用可能同步改 EFI，安装前后比较 config 与文件清单。 [1401](https://github.com/nullmoth/1401) · [OpCore-Simplify](https://github.com/lzhoang2801/OpCore-Simplify) |
| BOOTx64、OpenCore、OpenRuntime，Acidanthera | UEFI 入口、引导器、运行时服务。当前 Vault 为 Optional。 | 一次使用同版 `BOOTx64.efi`、`OpenCore.efi`、`Drivers/OpenRuntime.efi` 和该版 Sample.plist/ocvalidate。按新 schema 迁移，逐项复查 UEFI、Booter 和 NVRAM quirks。 |
| OpenCanopy，OpenCorePkg | 图形 picker，当前 External、显示菜单、5 秒倒计时，辅助项可见。 | 配套更新 `Drivers/OpenCanopy.efi` 与 `Resources/`。检查 Windows、恢复、正常启动和备用 EFI 项。 |
| HfsPlus，OcBinaryData 中分发 | HFS+ 引导文件系统读取。 | 核对 `UEFI → Drivers` 路径和架构，实测恢复介质识别。 |
| apfs_aligned，1401 基线随附 | 当前启用，基线文件一致。具体二进制构建来源待核实。 | 先查新 1401 的清单和 APFS 启动要求，确认 `UEFI → APFS` 与相关驱动；实测 APFS 启动卷与安装继续项。 |
| ResetNvramEntry，OpenCorePkg | 当前列在 UEFI Drivers。它会清空持久化变量。 | 与 OpenCore 配套；验证菜单里出现即可。真正重置前保存 BIOS、引导顺序与有效启动参数。 |
| NullMothSafe，1401/NullMoth | 初始副本无此文件，当前在 `Tools/NullMothSafe.efi`，配置有两个工具项。 | 按 1401 的配套版本检查 `Misc → Tools` 的两个项、指向的文件及参数，核对它们的用途后再决定是否保留各项。保留可从 Xe 启动的独立 EFI。 |

[OpenCorePkg 与配置手册](https://github.com/acidanthera/OpenCorePkg) · [OcBinaryData](https://github.com/acidanthera/OcBinaryData)

### 配置迁移时逐项对照

- `NVRAM → Add/Delete` 中 Apple 启动变量 GUID 下，本机最终配置以先删后加设置 `boot-args`，原始 Windows 基线未删除该变量。升级后同时确认目标 plist 和实际运行的参数。Xe/显示相关有 `-igfxdvmt`、`-nodisplaysleepDC6` 等；NullMoth 图形组有 `gfxrst=4`、`nvfb=1`、`nvaccel=1`、`nvfbheads=4`、`-nvkmsnosmooth`、`nvrmacpibl=1` 等；I2C 有 `-vi2c-force-polling`，蓝牙有 `-ibtcompatbeta`。跨版本检查每个选项的归属和支持状态。
- `Misc → Debug` 最终为 `Target=0`、`DisplayLevel=0`、AppleDebug/ApplePanic/SysReport 关闭，去掉独立 `-v`。现有 `DisableWatchDog=true` 保留自基线；没有实测证据把它和旧卡死连起来。排故采集需先确认目标 EFI 和时间窗，再暂时打开所需项。
- 当前安全相关配置是旧版本组合的一部分。迁移时逐项核对 `Vault`、`SecureBootModel`、`ScanPolicy`、`DmgLoading`、SIP、AMFI 实际状态与目标驱动说明。独立迁移信任链，保留回退 EFI 和系统组件副本。
- `PlatformInfo → Generic` 的型号为 MacBookPro16,2，机器标识由各使用者独立生成并私下保存。CPU 名称显示由 RestrictEvents/`revpatch` 等配置影响，实测睿频需要单独观测。
- 当前 `Kernel → Patch` 有三条按 Darwin 版本范围匹配的 HT 补丁。Tahoe 落在 `MinKernel=24.0.0` 一组；其余是旧系统项，最旧项注释标为未测。升级区别处理二进制补丁识别、CPU 拓扑和显示名称。不要仅凭注释判断命中。
- `DeviceProperties → Add` 按设备拆开核对。HDEF `PciRoot(0x0)/Pci(0x1F,0x3)` 的 `layout-id=13`，核显 `PciRoot(0x0)/Pci(0x2,0x0)` 有 framebuffer、stolenmem、`hda-gfx` 等属性，PEG 下有 `acpi-wake-type`。换 BIOS 或方案后先复查 PCI 路径及各属性的实际消费者。

## 3. 图形栈与 Xe 补丁移植

### WhateverXe 0.8.2 的上游源码修改

从 [b00t0x/WhateverXe](https://github.com/b00t0x/WhateverXe) 的 `0.8.2`（发布提交 `4512191`）取源码。实机当前 Xe 二进制 SHA-256 为 `888a4fb498159b4b14c0ed204cada7e4c8490e8c59ebba13320e3f7f5d735ce0`，与历史移植构建记录一致。补丁只涉及下表三处。 [本机补丁](../patches/WhateverXe-0.8.2-HP-Omen.patch)

| 原作者包内文件 | 本机具体变更 | 移到新版本时改动点 |
| --- | --- | --- |
| `WhateverXe/kern_vbt.hpp` | `panelFacts()` 与 `parse()` 增加缺省值为 `-1` 的 `panelOverride`；只有 VBT 的 panel byte 为 `0xFF` 且调用方明确选择 0，才把 panel type 解析为 0。其余长度、重复块、端口和有效性校验继续保留。 | 先找新版 VBT parser 和 panel block 解析，检查边界、checksum、eDP/power block 和端口映射的变化。重新测试有效索引、默认拒绝 `0xFF`、仅显式允许 0、截断及错误端口。 |
| `WhateverXe/kern_firmware.cpp` | 在 `XE::captureFirmware()` 检查 PCI `8086:46A6`/子系统 `103C:8A13`、OpRegion 8704、VBT 8647、版本 242、位图 `0x19`、端口 0 内置 DP、偏移 2198 为 `0xFF` 和原面板无效。二次解析仍要求有效 panel、DDI 0、同位图、同 checksum；成功发布 `WEXHPOmenPanelFallback`。 | 确认新版 OpRegion 映射长度与解析接口。BIOS 变化时重新读取并解析 VBT，逐项核实字段和偏移；继续限定到已验证板型。若新版已能识别本机 VBT，使用新逻辑做实机对照后再决定保留哪些差异。 |
| `WhateverXe/kern_xe.cpp` | `XE::wrapAlderLakeNSetupWorkLoop()` 的延后接管分支，为已通过 fallback 的 DDI0、有效 port 大小、原模式 0、GOP/boot DDI 受保护、AUX boot lease pending 且无 owner 的路径调用 `wrapDisplay13TCMode(port,1)`。调用时按当前实现管理对象引用和锁，记录 `WEXHPBootPortMode`，随后走上游 SST/AUX handoff。 | 重新核对当前类布局和字段偏移（旧为 `0x550`）、最小类大小（旧为 `0x624`）、回调时机、锁和 owner 状态。保持所有守卫。继续验证失败分支、非 Omen、非 DDI0 和已有 owner 情形；已有实机包未引入旧 P1/P2 背光或面板供电 hook。 |

实际更新顺序如下。

1. 固定新 tag 与依赖版本，阅读变更记录，先在新工作树跑 `git apply --check`。补丁即使能直接应用，也要核对三个改动点的语义和测试。重新编译 Release，检查 kext 版本、x86_64、Lilu 依赖、差异与哈希。
2. 替换测试盘 `Kexts/WhateverXe.kext`，保留 Lilu 先于 Xe，检查禁用的 WhateverGreen。配套升级系统侧 Graphics，并依据目标版本 README 选用对应信任链。
3. 从测试盘启动，核对 `kmutil showloaded`、`system_profiler SPDisplaysDataType`、`ioreg -l -w0` 的面板属性。验证内屏接管、实测亮度、Metal、外接输出、睡眠唤醒，再测 RTX 3060 启动和原有音频。

### 系统图形组件、信任和 RTX 3060

| 组件与来源 | 当前记录与本机改动 | 后续升级落点 |
| --- | --- | --- |
| WhateverXe Graphics，b00t0x | 历史组合 Graphics 0.4.4、AppleIntelTGLCompat 0.8.1、用户态 16.0.32；运行态确认 Compat 0.8.1。VA 当前以 `.bundle.disabled` 留存，旧 B 组脚本不属于当前方案。 | 跟随 Xe release 兼容矩阵。盘点 `/Library/Extensions/AppleIntelTGLCompat.kext`、AppleIntelTGLGraphics/Framebuffer 和 `/Library/GPUBundles/AppleIntelTGLGraphics{GL,MTL,VA}Driver.bundle`，核查目标版本实际安装清单、签名、收据和 AuxKC。保留原包，按上游处理 VA。 [Graphics 项目](https://github.com/b00t0x/WhateverXe-Graphics) |
| AMFIPass 1.4.1，DhinakG 系列 | 当前 EFI 文件与 Windows 基线一致，且运行态已加载；历史 Graphics 路线采用 AMFIPass。 | 新版上游说明已采用 AMFIAnchor 体系时，先核对当前 SIP 和 AMFI 参数，再按对应版本部署系统工具及 EFI Injector，复查 AuxKC 和签名。旧 `amfi=0x80`、`ipc_control_port_options=0` 等不作为新路线通用要求。 [WhateverXe 安装说明](https://github.com/b00t0x/WhateverXe#installation) · [AMFIPass](https://github.com/dhinakg/AMFIPass) |
| NVRM、NVRMFB、NVRMAGDC、NVAccel、NMXAudio，NullMoth | 运行态已核对前四个均为 3.0，系统卷还有 NMXAudio。未发现本项目修改其内核源码的补丁。实机 3060 有独显直连成功记录。 | 更新 `/Library/Extensions` 下这组 kext 和上游安装清单所列的用户态、服务、EFI 参数与工具。安装 Xe Graphics 也会重建共用 AuxKC，更新前后都检查两套驱动。先混合，后直连，验证内外屏、Metal、冷启动和睡眠。 [NullMoth NVIDIA](https://github.com/nullmoth/nvidia-macos-driver) |

维护时把 NullMoth 五个系统文件分别列入备份和安装后核查。

| 文件 | 核查重点 |
| --- | --- |
| `NVRM.kext` | GPU 设备与驱动核心是否初始化，确认 GA106/设备 ID、配套 NVIDIA 用户态依赖和上游支持矩阵。 |
| `NVRMFB.kext` | 显示器 head、EDID、模式设定及帧翻转。直连内屏和外接输出分别验证。 |
| `NVRMAGDC.kext` | 配套显示管理及依赖是否随版本正确加载，结合实际主屏归属检查。 |
| `NVAccel.kext` | 对照 Metal/IOAccelerator、窗口渲染和实际图形负载，检查桌面帧启动阶段。 |
| `NMXAudio.kext` | 单独核对 NVIDIA 显示音频的加载和 HDMI/DP 输出。内建 ALC245 和 Intel DMIC 分开验证。 |

RTX 黑屏专项记录为 11 月 23 日晚，日志只保留了 20:27 后切回 Xe 的启动。另一次凌晨直连记录有 EDID、1920×1080 mode commit 和持续 flip，但不能与晚间故障合并。仍可见指针让桌面帧交接和扫描输出值得排查；`-nvrmnoflip` 是单变量测试并留在当前 EFI。若复现，按同一固件模式记录时间、主屏、指针能否移动、关机方式，对照 WindowServer、NVRMFB mode/head/flip 与 NVAccel；未经同条件前后对照不宣称已修复。 [NullMoth 排障](https://github.com/nullmoth/nvidia-macos-driver/blob/main/docs/TROUBLESHOOTING.md)

## 4. 音频与麦克风

| 组件与来源 | 本机改动和版本 | 升级要改动的位置 |
| --- | --- | --- |
| AppleALC 1.9.8，Acidanthera | 从初始 1401 补入，配合 Lilu 和 `HDEF layout-id=13` 实测内建扬声器出声。没有本机 AppleALC 源码改动记录。 | 更新 `Kexts/AppleALC.kext` 与 Lilu，核对新版 ALC245 13 号 layout 和 HDEF PCI 路径。先测扬声器，再测耳机与插拔，单独验 DMIC。 [AppleALC](https://github.com/acidanthera/AppleALC) |
| SimpleHDA 1.2.0，基于 laobamac | 上游固定提交 `18a16d43b85fd90118ab7307f92a8b46aecd501a`。1.2.0 集成 AppleHDADmicBridge 到载荷，同时保留模拟输出所需的原版 AppleHDA。 | 在新上游工作树移植下列六处；以完整依赖和原始 AppleHDA 文件构建。替换 EFI 的 `Kexts/SimpleHDA.kext`，同步升级同 ABI 的 DMIC helper 和 HAL。 [上游](https://github.com/laobamac/SimpleHDA) · [集成差异](../patches/SimpleHDA-OMEN-DMIC.patch) |
| AppleHDA 600.2 与 AppleHDAHALPlugIn，Apple macOS 15 / KDK 24H23 | 原版载荷在本地构建时校验并生成，原版 HAL 安装位置为 `/Library/Audio/Plug-Ins/SimpleHDA/AppleHDAHALPlugIn.bundle`。 | 更换 AppleHDA 版本时核对 x86_64、原版签名、文件清单、kext 依赖、HAL 路径以及 AppleHDA 私有结构/API 与 helper 用法。按版本更新输入 manifest，完整重建载荷。 |
| AppleHDADmicBridge 与 SOF IPC4，本项目 + SOF/Linux 参考 | 本机 ADL cAVS 2.5、SOF v2.12 Intel 签名固件、对应双通道拓扑和 NHLT。helper 由 SimpleHDA 嵌入，配合 AppleHDA 生命周期。 | 保持 AppleHDA 唯一 PCI 所有权。升级 SOF 前比对 ADL 型号、IPC4 ABI、固件哈希、模块/拓扑、NHLT、pipeline/控制参数；重新生成图规划与固件头。变更队列、stream descriptor、传输或中断路径后做硬件回归。 [SOF 固件](https://github.com/thesofproject/sof-bin) · [Linux SOF](https://github.com/torvalds/linux/tree/master/sound/soc/sof) |
| HPIPC4Audio HAL，本项目，基于 Apple AudioServerPlugIn 示例 | 作为 `/Library/Audio/Plug-Ins/HAL/HPIPC4Audio.driver` 提供独立内建输入。对应用公开单一 `imic` 数据源，解决通用重复数据源项所暴露的可见性和属性问题。 | 更新 `AudioServerPlugIn/HPIPC4AudioPlugIn.c`、`Info.plist` 与 DmicAudioBridgeABI 时，同时更新内核桥和 HAL。核对工厂 UUID、设备 UID、内建传输、默认输入资格、时钟与采样规格。把旧副本移出 HAL 目录，重启后验证真正产出样本。 |

### SimpleHDA 原作者包内的改动清单

| 文件 | 当前修改 | 升级移植重点 |
| --- | --- | --- |
| `Sources/kern_start.cpp` | 扩充载荷启动和设备注册。在有自定义 DMIC 模块时链接和注册模块，处理其匹配字典；以原 AppleHDA 所有权作为边界。 | 确认新版 payload/加载顺序、匹配与精确 provider，保留无 helper 时的原版路径，检查重复注册和失败清理。 |
| `Tools/prepare_payload.py` | 校验并纳入 `Resources/AppleHDADmicBridge.kext`，生成额外模块记录和内容。 | 对照生成结构、模块命名、依赖符号、bundle 路径和版本，保持原 Apple 载荷校验。 |
| `Tools/verify.py` | 加强版本、kmod 和未解析符号/内嵌模块检查。 | 保留原版校验并验证新 helper 被嵌入，也能在没有嵌入时识别产物差异。 |
| `Info.plist` | 当前自定义版号 1.2.0。 | 与 `CFBundleVersion`、`CFBundleShortVersionString` 和 kmod 元数据统一；OpenCore 保持相同 bundle、执行文件和 plist 路径。 |
| `Build/ManualKmod.cpp`、`Build/relink.zsh` | 历史增量构建入口和 kmod 元数据。 | 如使用该入口，核对脚本版本宏、完整对象集合和链接标记；发布前优先做干净 Xcode 构建，避免把旧对象链接进新版本。 |

内核桥关键文件以 `src/dmic-bridge/` 为根。SOF 启动、流获取、启停和失败隔离主要在 `HPIPC4Controller.cpp`、`AppleHDAStreamLease.*`；IPC 消息、图实例和任务顺序在 `DmicKernelOperation*`、`DmicKernelSession.*` 等；采集发布与中断位置在 `DmicCaptureInterrupt*`、`DmicCapturePeriod*`、`DmicSharedCaptureRing.*`；用户态约定在 `DmicAudioBridgeABI.h` 和 `AudioServerPlugIn/HPIPC4AudioPlugIn.c`。升级这些文件时按调用链做成组测试，避免只换 HAL 或只换内核模块而让 ABI 不一致。

早期独立 HPIPC4 曾收到 FW_READY、双通道配置及 RUN，应答成功以后仍有 DeviceBusy 与 DMA/HAL 回归。AppleHDA 接管后，重复复用已停止的输入流时，AppleHDA 会重置 host DMA 游标，SOF 必须一起丢弃上一轮传输状态。试验中曾出现四个通用数据源、刚开机有输入稍后消失、普通录音与 Siri 一项恢复另一项退化。由此把「设备注册」「实际采样」「停止后重新开始」设为三个独立验收点。当前 `resetStoppedCaptureSession()` 负责 DSP 侧复位，`rebaseCapturePositionBeforeInterruptEnable()` 建立位置/轮询基线，再放行中断与样本发布。仅看设备枚举、名称或 FW_READY 都无法证实听得到录音。

验收依次确认扬声器、内建输入电平、QuickTime 实录回放、多轮打开与关闭、等待几分钟后重录、Siri 单独采集及睡眠唤醒。故障定位顺序为 AppleHDA 所有权 → DSP FW_READY → IPC pipeline → DMA position/IOC → ring 序号 → HAL 的 CoreAudio 属性和实测样本。HAL 离线 smoke test 只验证工厂与 `imic` 属性，不能替代录音。

## 5. 输入、USB、网络、蓝牙和基础 kext

「与初始盘一致」表示该 bundle 和初始 EFI 副本的可见文件一致。「替换」表示核对到文件差异或新加入，具体源码提交如无证据则保留为未核实。以下 kext 均在 `Kexts/`，对应 `Kernel → Add`；嵌套组件须保留完整相对路径。

| 组件、版本和项目 | 实际做了什么 | 后续升级落点和验收 |
| --- | --- | --- |
| Lilu 1.7.3，Acidanthera | 较初始盘替换；为 WhateverXe、AppleALC 等补丁插件提供基础。 | `Lilu.kext` 先于其插件，核对最小版本和 Darwin 兼容。确认插件真正加载。 [Lilu](https://github.com/acidanthera/Lilu) |
| VoodooI2C 2.9.1，VoodooI2C | bundle 较初始盘有变化，触控板已恢复。具体上游提交未核实。 | 更新父 kext 与兼容的插件；比较 `Info.plist` IOKit personality、控制器匹配、GPIO 中断和轮询选项，注意 `-vi2c-force-polling`。 [VoodooI2C](https://github.com/VoodooI2C/VoodooI2C) |
| VoodooI2CServices 1、VoodooGPIO 1.1，同包插件 | 当前从 VoodooI2C 的 `Contents/PlugIns/` 启用，提供服务和 GPIO。 | 与 VoodooI2C 整包更新。核对嵌套路径和依赖，按实际 ACPI GPIO 资源走中断或轮询，醒后实测。 |
| OmenI2C-51E8 1.0.0，本机 codeless 注入 | 新增 `Contents/Info.plist`，`IOPCIMatch=0x51E88086`，使用上游 `VoodooI2CPCIController` 类。时钟 133 MHz，SCL/SDA falling 和 SDA hold 为 208/171/42 ns。 | 首查新 VoodooI2C 是否原生匹配 51E8、类名是否变化、timing 键是否仍被读取。保留单一 personality，验证触控板识别、点按、手势和睡眠。 [本机 Info.plist](../efi/OmenI2C-51E8.kext/Contents/Info.plist) |
| VoodooI2CHID 1，VoodooI2C | 较初始盘替换，负责 HID-over-I2C 触控板 `373B:1012`。 | `VoodooI2CHID.kext` 跟随父驱动的兼容版本，核对 HID 设备匹配、GPIO/轮询、键盘并行工作情况。 [VoodooI2CHID](https://github.com/VoodooI2C/VoodooI2CHID) |
| VoodooInput 1.1.7，Acidanthera | VoodooI2C 内嵌副本启用，PS2 内嵌副本禁用。 | 更新后再查两个插件 Add 项和 Bundle ID，保持只有一个 Input 实现，验证手势和启动。 [VoodooInput](https://github.com/acidanthera/VoodooInput) |
| VoodooPS2Controller 2.3.8，Acidanthera | 本体、`VoodooPS2Keyboard.kext`、`VoodooPS2Mouse.kext`、`VoodooPS2Trackpad.kext` 启用，bundle 与初始盘一致。 | 随同版本更新本体和三插件，分别测试内建键盘、功能键、指针与点按。两套 VoodooInput 的规则不变。 [VoodooPS2](https://github.com/acidanthera/VoodooPS2) |
| USBToolBox 1.2.0，USBToolBox | 与初始盘一致，提供 USB 端口映射机制。 | 更新 `USBToolBox.kext` 后验证与 map 格式的兼容，`XhciPortLimit` 保持关闭或按新版说明核对。 [USBToolBox kext](https://github.com/USBToolBox/kext) |
| UTBMap 1.2.0，本机 USBToolBox 映射 | 新增并启用 `UTBMap.kext`；初始盘的 UTBDefault 文件仍在，当前配置未登记。 | 用 USBToolBox 在 Windows 重测 USB 2/3、Type-C 两向及内建蓝牙，必要时重做 `UTBMap.kext/Contents/Info.plist` 中的 controller 和 ports。不要与默认全开映射同时使用。 [映射工具](https://github.com/USBToolBox/tool) · [本机映射](../efi/UTBMap.kext/Contents/Info.plist) |
| XHCI-unsupported 0.9.2，RehabMan injector 标识 | 与初始盘一致，用于补控制器匹配。 | 查看自身 `Info.plist` 的 PCI match 和新系统原生匹配后才决定保留或更新；分别测速验 USB2/3 和唤醒。 [RehabMan 注入器项目](https://github.com/RehabMan/OS-X-USB-Inject-All) |
| AirportItlwm 2.4.0，OpenIntelWireless itlwm 路线，早期记录引用 laobamac Tahoe 分支 | 增配 AX211，用户曾确认 Wi-Fi 可用，当前运行态已加载。现有记录未锁定该二进制源码提交。 | 替换 `AirportItlwm.kext` 之前确认其面向实际 Darwin/IO80211 版本，保留现有可用包。若采用 itlwm + HeliPort，需要按另一工作模式配置并避免双重加载。实测联网、重连和睡醒。AirDrop/AWDL 另验。 [OpenIntelWireless](https://github.com/OpenIntelWireless/itlwm) · [Tahoe 分支参考](https://github.com/laobamac/itlwm) |
| IntelBluetoothFirmware 2.5.2、IntelBTPatcher 2.5.2，OpenIntelWireless | 两个 bundle 较初始盘有变化，针对 Intel USB 蓝牙 `8087:0033`。历史用户确认蓝牙可用。 | 按上游说明成套更换两 kext，核对 USB 内建标记、加载顺序、服务及蓝牙固件状态，实测冷启动配对和唤醒重连。 [IntelBluetoothFirmware](https://github.com/OpenIntelWireless/IntelBluetoothFirmware) |
| BlueToolFixup 2.7.3，Acidanthera BrcmPatchRAM 发布包 | 与初始盘一致；和 Intel 蓝牙栈、Lilu、Tahoe 的 `-ibtcompatbeta` 配套。 | 在对应 macOS 版本核对上游组合和参数是否仍需，换件后不要凭蓝牙图标判断控制器已工作。 [BrcmPatchRAM](https://github.com/acidanthera/BrcmPatchRAM) |
| RealtekRTL8111 2.4.2，Mieze | 与初始盘一致，为匹配 Realtek PCI 的有线网络提供驱动。 | 核对 `RealtekRTL8111.kext` 兼容 NIC ID、DHCP、吞吐、恢复环境与唤醒。 [RTL8111](https://github.com/Mieze/RTL8111_driver_for_OS_X) |
| VirtualSMC 1.3.9，Acidanthera | 与初始盘一致，提供 SMC 模拟。 | 更新 VirtualSMC 和兼容插件，核对 SMC 加载、启动、休眠与恢复。 [VirtualSMC](https://github.com/acidanthera/VirtualSMC) |
| SMCBatteryManager、SMCLightSensor、SMCProcessor、SMCSuperIO，均 1.3.9 | 与初始盘一致，分别承担电池、环境光、CPU 指标和部分传感器。OMEN MUX 的监控另有取数路径。 | 四个独立 `.kext` 一一替换并对齐 VirtualSMC 发行版本。逐项验证电池/充放电、电源状态、光感是否有实体设备、CPU 和传感器读数。某文件存在不意味着对应硬件能读数。 |
| ECEnabler 1.0.6，1Revenger1 | 与初始盘一致，EC 访问适配。 | 更新后核对 `SSDT-EC` 和 EC 路径、电池数据与唤醒；保持 EC 注入链一致。 [ECEnabler](https://github.com/1Revenger1/ECEnabler) |
| NVMeFix 1.1.4，Acidanthera | 与初始盘一致，NVMe 电源管理兼容组件。 | 配对 Lilu，按 SSD 具体型号和目标 macOS 检查功耗、睡眠唤醒和磁盘错误。 [NVMeFix](https://github.com/acidanthera/NVMeFix) |
| RestrictEvents 1.1.7，Acidanthera | 与初始盘一致。当前配 `revpatch=sbvmm,cpuname`。 | 更新后对照上游参数、SMBIOS 分支和升级需求。另行检查 CPU 名称、升级可见性及真实性能。 [RestrictEvents](https://github.com/acidanthera/RestrictEvents) |
| BrightnessKeys 1.0.4，Acidanthera | 与初始盘一致，处理亮度键；实际背光还依赖 PNLF 与活动 GPU。 | 同时核对 Lilu、`SSDT-PNLF` 与键盘事件。测按键、系统滑块和物理亮度变化。 [BrightnessKeys](https://github.com/acidanthera/BrightnessKeys) |

## 6. ACPI 表和重命名

十张启用的 SSDT 都可在初始 1401 产物找到对应文件，OEM ID 字段为 ZPSS。参考源为 1401 所用 OpCore-Simplify 生成流程。对照发现 PNLF 两个字节差异落在 ACPI checksum 和 creator revision 字段，AML 正文一致。其他九张文件字节一致。迁移时把原生 DSDT 和新增 SSDT 在同一命名空间核对，保存生成规则和 ASL，重新编译后再替换目标 AML，并检查 `ACPI → Add/Patch`。 [生成器](https://github.com/lzhoang2801/OpCore-Simplify) · [ACPICA](https://github.com/acpica/acpica)

| 表或补丁 | 当前用途 | 升级检查点 |
| --- | --- | --- |
| `SSDT-EC.aml` | 注入 macOS 所需 EC 定义。 | 对照原生 EC 名称、设备路径和冲突，联合 ECEnabler 和电池验证。 |
| `SSDT-USBX.aml` | USB 电源属性注入。 | 核对供电属性和型号，回归充电、休眠、唤醒。USB 端口拓扑由 UTBMap 维护。 |
| `SSDT-GPI0.aml` + GPI0 `_STA → XSTA` | 控制 GPIO 可见性，支持 I2C 路径。 | 检查原生设备名、补丁作用范围和实际命中。成对验证 VoodooGPIO、触控板及中断。 |
| `SSDT-PLUG-ALT.aml` | 针对处理器命名空间的 plug 属性注入。 | 换 BIOS 时核对 CPU 路径与 processor 对象，检查电源管理服务，实测频率、功耗、睡眠。 |
| `SSDT-MCHC.aml` | 提供 MCHC 设备定义。 | 核对 root bridge 与已有节点，避免重复定义。 |
| `SSDT-SBUS.aml` | 提供 SMBus 相关设备定义。 | 与原生 SBUS 路径和系统服务匹配，检查是否已有兼容设备。 |
| `SSDT-RTCAWAC.aml` | 为 RTC/AWAC 兼容性提供 ACPI 条件。 | 核对固件 RTC/AWAC 与开机时钟，单独验证冷启动、睡眠和 CMOS 状态。 |
| HP RTC Power Loss (005) 补丁 | 当前启用的 HP RTC 兼容重写。 | 在 `ACPI → Patch` 对照 Find/Replace、表条件和命中范围。换 BIOS 不复用未知字节序列。 |
| `SSDT-USB-Reset.aml` | USB 控制器重置/枚举适配。 | 复查 XHCI 命名、端口清单、恢复介质读盘和睡醒。 |
| `SSDT-XOSI.aml` + `_OSI → XOSI` | 成对处理操作系统判断方法。 | 检查重命名影响哪些表，核对 I2C/触控板依赖的分支；变动后复测其他 ACPI 设备。 |
| `SSDT-PNLF.aml` | 声明 `_SB.PC00.GFX0.PNLF`，`_UID=0x13`。 | 新版 Xe 与 BIOS 组合先检查真实核显路径、PNLF 是否已存在、上游 UID 约定。检查系统滑块和实际面板亮度，区分 Xe 与 3060 直连。 |

## 7. OMEN MUX 固件控制项目

已安装应用为 `/Applications/OMEN MUX.app` 0.2.6，盘上 EFI 为 `Kexts/OmenMuxProbe.kext` 0.1.9。本轮运行态查询未列出 OmenMuxProbe，因此本节实测结论引用 MUX 交接，盘上 0.1.9 需从目标盘启动后再次核对。源码在 `src/omen-mux/`。参考了 HP OMEN SDK、本机 Windows OMEN Gaming Hub 模块和 Linux HP WMI；本项目独立实现了 IOKit ACPI 调用及 SwiftUI 前端。 [HP SDK](https://gaminghub.omen.com/documents/static/ReleaseNotes/index.html?theme=white) · [Linux HP WMI](https://github.com/torvalds/linux/blob/master/drivers/platform/x86/hp/hp-wmi.c)

| 文件 | 实现与本机实测 | 升级要点 |
| --- | --- | --- |
| `OmenMuxProbe.cpp`、`Info.plist` | 从 `PNP0C14` 的 `_WDG` 按 HP WMI GUID 找到方法，本机为 WMAA。MUX `0x52`，性能 `0x4C` 写后读回，风扇数量/转速/全速状态/写入分别为 `0x10/0x11/0x26/0x27`。串行访问固件。仅接受指定模式和风扇请求。 | 换 BIOS/机型先构建 `read-only`，验证 GUID、方法、档位编码、风扇数量以及读回。切换只在对应板型和固件验证后启用。升级 kext 后重启，从正确测试盘核对运行版本。 |
| `MuxService.swift`、`main.swift` | 客户端筛选有 `OmenMuxMethod` 的 IOKit 服务。CLI 提供 `status`、`firmware`、`metrics` 和控制接口。 | IORegistry 属性名、值和控制 ABI 变更时同步驱动、CLI 和 UI。确认普通用户下读写以及多同名 ACPI 服务过滤。 |
| `MuxWindow.swift`、`AppInfo.plist` | 固件接受图形模式后提示立即或稍后重启，重启后由实际面板归属确认。稍后重启不会撤销已提交的模式。 | 纯 UI 升级可独立替换 app。若固件操作、CLI 协议或属性变化则配套升级。验证取消弹窗、待重启状态、系统重启授权和固件读回。 |
| `Metrics.swift`、`SMCTemperature.swift` 和驱动采样 | Mach CPU/VM、IOAccelerator PerformanceStatistics；驱动按 CPUID 和型号守卫读取封装温度 MSR `0x1B1/0x1A2`，应用留 SMC 后备。 | 核对目标 GPU 的统计键、传感器缺失状态和 CPU 能力守卫。验证两路风扇和温度确实刷新，驱动轮询和 UI 更新互不阻塞。 |
| `build.sh` | 应用内置 CLI，有 `read-only` 和 `switch` 变体，构建产物 ad hoc 签名。 | 编译后验目标架构、版本和签名。普通用户进程能调用驱动暴露的固定控制项，审查权限边界及允许值。 |

实测三档编码为 `0x00/0x01/0x02`，风扇全速标志 `0 → 1` 时曾从约 2409/2220 升至 5500/5719 RPM，恢复自动后回落。验证 MUX 时核对下次启动后的主屏归属；仅看到两个 GPU 或看到「已提交」都不足以确认固件切换成功。构建与部署见 [构建说明](build.md)。

## 8. 仍在盘内的备用或历史组件

| 组件与来源 | 当前用途和维护决策 |
| --- | --- |
| WhateverGreen，Acidanthera | `Kernel → Add` 禁用且没有活动 bundle。独立对照上游方案才单独启用，避免与 WhateverXe 并行。 [WhateverGreen](https://github.com/acidanthera/WhateverGreen) |
| CpuTopologyRebuild 2.0.2，b00t0x | 文件与原始 1401 一致，Add 禁用。若研究混合核心调度，先核对现有 HT kernel patch、SMBIOS、目标 Darwin 和上游启动参数，再用独立测试 EFI 验证。 [CpuTopologyRebuild](https://github.com/b00t0x/CpuTopologyRebuild) |
| HPDMICProbe 0.2.0，本机探针 | 当前禁用；历史只读诊断不参与最终采集。排障时另用隔离配置和指定探针，按 SoC、BAR 和所有权守卫验证。 |
| HPIPC4 0.1.0 与 SharedOpenProbe，本机实验 | 前者在当前盘但禁用，后者无当前启用项。独立 PCI 版本曾与 AppleHDA 冲突，最终保留的桥由 SimpleHDA 嵌入。 |
| HPFWProbe、本机离线 IPC3 与 XPS/RT714 SoundWire 研究 | 用于结构参考。当前实机是 IPC4 + PDM；不用其他板型的二进制代替本机链路。 |
| UTBDefault 与 PS2 的 VoodooInput | 见第 5 节。分别避免默认/本机双映射及两个 Input 插件重复注入。 |

## 9. 每次升级的操作和回退顺序

1. 确定测试 U 盘、启动选项、系统构建和当前生效驱动。保存旧 EFI 的整树、版本、哈希；涉及图形或音频时另清点系统侧包和 AuxKC，确保备用盘实际可进系统。
2. 本机源码组件按对应章节移植，通用组件用各自的上游版本与依赖。用同版 `ocvalidate` 验配置，再核对所有 Enabled 项可找到文件，架构、Kernel 范围、依赖、顺序、plist 的 Bundle ID 正确。
3. 每次只更新一个组件族，记录涉及的 EFI 文件和系统文件，更新前后看 `kmutil showloaded`、IORegistry、系统报告和该功能的实际行为。需要重启时先保存工作，手动从指定盘启动，再记录启动结果。
4. 先测冷启动、Windows/恢复菜单和基本输入；图形档用混合和独显分别测试主屏、外屏、亮度与休眠；声卡测输出、反复录音和 Siri；网络/蓝牙逐个测试连接和恢复；USB 逐口测试；MUX 检查写后读回和重启后面板归属。
5. 启动失败先用明确的已知可用盘恢复。EFI 回退和系统组件回退分别执行，配套还原 Graphics、NullMoth、HAL 或 AuxKC。保留故障时段的局部日志及对照配置，隐藏序列号、账号、磁盘唯一标识和用户路径后再分享。
