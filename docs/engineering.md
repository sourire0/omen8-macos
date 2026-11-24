# 修复过程和验证记录

## Windows 基线到稳定启动

Windows 设备枚举显示实体麦克风阵列通过 Intel SST，端点为 `8086:AE20`；控制器为 Alder Lake cAVS `8086:51C8`，目标板子系统 `103C:8A13`，双声道 48 kHz。Realtek ALC245（`10EC:0245`）提供模拟音频。Windows 报告里的 10.29.0.7767 是 SST 驱动版本。相同声卡控制器下的两个音频路径需要分别验收。

测试期两份 U 盘 EFI 有过混用，MULL 的部分配置存在卡死或开不了机，1401 在 Windows 重建后也曾因变更失去启动能力。恢复 1401 启动后，将实验集中在该盘，保留独立回退介质。多个故障没有单一根因证据。配置写入、驱动加载、功能可用分别需要记录。

## Xe 内屏与 RTX 3060

WhateverXe 0.8.2 的本机补丁允许严格匹配 `8086:46A6/103C:8A13` 和特定 VBT 的 Omen 机型在 panel 字段为 `0xFF` 时验证候选索引 0。补丁检查 checksum、端口图和 DDI 0，并只在受保护的 GOP/AUX 接管条件下修正端口模式。它仍走上游 AUX/SST 接管流程。0.8.2 历史组合使用 Graphics 0.4.4 和 AppleIntelTGLCompat 0.8.1。

NullMoth 3.0 提供 RTX 3060 Laptop（GA106，`10DE:2520`）组件。混合模式下 Iris Xe 驱动 1080p 内屏，独显直连模式下也实测 RTX 驱动内屏。此前曾在进度条后只见鼠标指针。故障时段缺 NVIDIA 启动日志；另一次直连日志有 EDID、1920×1080 模式提交和翻转记录。桌面帧提交或扫描输出是待确认的环节。`-nvrmnoflip` 加入过 1401 作为测试变量，现有记录不足以把成功切换归因于它。 [NullMoth 排障](https://github.com/nullmoth/nvidia-macos-driver/blob/main/docs/TROUBLESHOOTING.md)

## Intel DMIC

| 阶段 | 发现或验证 |
| --- | --- |
| 设备枚举 | AppleALC 路线已有 ALC245 扬声器输出。通用输入设备名和滑块不能证明 PDM 麦克风在产出样本。 |
| SOF IPC4 | 对照 ADL SOF v2.12、对应拓扑与 NHLT，确认 FW_READY、两路 DMIC 配置、pipeline RUN 等进展。独立 PCI 实验出现 DeviceBusy，也与 AppleHDA 争用控制器。 |
| 控制器所有权 | 由 AppleHDA 单独持有 HDA PCI。SimpleHDA 将 AppleHDADmicBridge 装入自己的载荷作为子服务，协调 stream lease、DMA、IPC4 和 HAL 采集。 |
| 回归 | 一度出现启动刚完成能录音、等待或重开后消音；Siri 可见性和普通录音也有过交替回归。重复打开、冷启动后等待和 Siri 单独采集都进入验收。 |
| 最终实测 | SimpleHDA 1.2.0 配套独立 HPIPC4Audio HAL。源码包含采集启停游标同步处理和 AppleHDA 侧重建基线。HAL 提供逻辑内建源 `imic`。最终用户确认输入电平、QuickTime 回放与 Siri 在重启后恢复。等待后长周期和跨版本耐久测试无独立记录。 |

采集链为 **PDM 阵列 → cAVS/SOF IPC4 → HDA capture DMA → 共享环形缓冲 → HPIPC4Audio HAL → CoreAudio 应用**。出现名称但无电平时，依次检查固件就绪、pipeline 状态、DMA 位置与中断计数、环形缓冲序号、HAL 采样属性。遇到首次可用而复开失败时，重点比较每次 StartIO/StopIO 的硬件游标、DSP 已完成位置和驱动读指针。

## OMEN MUX

0.2.6 SwiftUI app 内置 `omen-mux` CLI。客户端查找带 `OmenMuxMethod` 的 IOKit 服务，通过 IORegistry 读取状态；驱动 0.1.9 按 HP WMI GUID 从 `PNP0C14._WDG` 找到方法，本机为 WMAA。控制和轮询使用同一锁。

| 功能 | 协议及验证 |
| --- | --- |
| 显卡模式 | WMI `0x52`；混合与独显提交后提示重启，已分别确认 Xe 和 RTX 驱动内屏。 |
| 性能档 | `0x4C` 写后读回，性能/平衡/低温为 `0x00/0x01/0x02`；与 Windows ECO 的功耗策略是否一致仍待测。 |
| 风扇 | `0x10` 数量、`0x11` 转速、`0x26` 全速状态、`0x27` 写全速或恢复自动。两路曾从约 2409/2220 升到 5500/5719 RPM，恢复后下降。 |
| 监控 | Mach CPU/VM、IOAccelerator PerformanceStatistics；匹配 CPU/CPUID 后读封装温度 MSR `0x1B1/0x1A2`，AppleSMC 为后备。 |

普通本地进程可调用驱动接受的固定请求。测试未知固件先用只读构建。交接读回为混合、平衡、自动风扇。重启提示中的「稍后重启」仅关闭提示，固件已接收待切换目标。 [HP SDK 发布说明](https://gaminghub.omen.com/documents/static/ReleaseNotes/index.html?theme=white) · [Linux HP WMI](https://github.com/torvalds/linux/blob/master/drivers/platform/x86/hp/hp-wmi.c)
