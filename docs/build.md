# 源码构建和部署

准备 Intel macOS 构建机、Xcode 或带 Kernel.framework 头文件的 Command Line Tools，以及 Python 3、Git、make。编译与静态检查只在工作目录进行。部署前单独备份启动介质和系统侧组件。

## OMEN MUX

在仓库根目录运行以下命令。先用默认只读版本调查未知固件，确认支持后用 switch 变体。公开版本使用系统默认图标，构建时可自行提供 `assets/AppIcon.icns`。

```sh
cd src/omen-mux
sh build.sh read-only
sh build.sh switch
codesign --verify --deep --strict 'build/switch/OMEN MUX.app'
codesign --verify --strict build/switch/OmenMuxProbe.kext
```

复制应用到 Applications；驱动放到测试 EFI 的 OC/Kexts，并按 EFI 片段登记 OpenCore 的 Kernel/Add。重启并确认 `kmutil showloaded | grep OmenMuxProbe`。应用包中的 `Contents/MacOS/omen-mux` 支持 `status`、`firmware` 和 `metrics`。移除时先重启到已知可用的配置，再停用 kext。构建产物使用 ad hoc 签名。

## DMIC 与 SimpleHDA 1.2.0

在本仓库根目录准备上游工作树。SimpleHDA 原项目版权声明保留，集成差异以补丁提供。

```sh
git clone https://github.com/laobamac/SimpleHDA.git src/SimpleHDA
git -C src/SimpleHDA checkout --detach 18a16d43b85fd90118ab7307f92a8b46aecd501a
git -C src/SimpleHDA apply --check ../../patches/SimpleHDA-OMEN-DMIC.patch
git -C src/SimpleHDA apply ../../patches/SimpleHDA-OMEN-DMIC.patch
```

本地准备原始 macOS 15 AppleHDA 600.2（历史来源 KDK 24H23）和 ADL SOF 2.12 Intel 签名固件。先核对 AppleHDA 文件与上游 `Resources/AppleHDA.json`，把完整 bundle 放在 `src/SimpleHDA/Resources/AppleHDA.kext`，完整构建的上游校验会读取这个固定位置。SOF 从上游对应发布版本取得并核对 SHA-256 为 `351325786e53ca7cd23ffc1462d82913f3e0c08ad78f89053d85e4b6703cf12b`。项目包含本机图规划源文件，固件头由构建时生成。 [SOF 固件发布](https://github.com/thesofproject/sof-bin/releases)

```sh
cd src/dmic-bridge
FW='/absolute/path/to/sof-adl-ipc4-intel-signed-v2.12.ri'
python3 verify_firmware_digest.py "$FW" 351325786e53ca7cd23ffc1462d82913f3e0c08ad78f89053d85e4b6703cf12b
make KERNEL_SDK="$(xcrun --show-sdk-path)" DMIC_FIRMWARE="$FW" appledha-dmic-module hal-plugin hal-factory-smoke
cd ../SimpleHDA
zsh Tools/prepare.zsh
zsh Tools/build.zsh Release
```

HAL smoke test 加载本地 bundle 并枚举工厂和 `imic` 属性，未开启硬件采集。部署时将 Release 的 SimpleHDA.kext 加到测试 EFI；将 `src/dmic-bridge/build/HPIPC4Audio.driver` 安装到 `/Library/Audio/Plug-Ins/HAL`，采用 root:wheel 所有权，确认插件可执行文件及目录权限，再重启。旧 HAL 版本和备份移出系统 HAL 目录。AppleHDA 原版 HAL 与自建 HPIPC4Audio HAL 是不同组件，前者按 SimpleHDA 文档部署。独立 HPIPC4 PCI 匹配驱动不参与本方案。

拓扑研究脚本位于 `src/dmic-bridge/audit`。复现离线审计还需要本机导出的 NHLT、对应上游拓扑和由提取脚本生成的候选拓扑。使用 `make offline-audit` 时显式传入 `DMIC_NHLT`、`DMIC_SOURCE`、`DMIC_CANDIDATE` 和 `DMIC_FIRMWARE`。这些输入与日常构建使用的已生成图规划分开维护。

## EFI 和图形依赖

`efi/OMEN8-reference.plist` 记录 ACPI/Booter/DeviceProperties/Kernel/UEFI 和 SMBIOS 型号，供逐项对照。`efi/` 另收录 51E8 注入和本机 USB 映射的无可执行文件 kext。其余组件从各自上游取得。Xe 补丁以 WhateverXe 0.8.2（4512191）为基线，先 `git apply --check`，按该版本说明构建。升级到其他版本时审查三个补丁点和运行态匹配守卫。

公开包收录自定义源码、补丁、配置片段及测试摘要。AppleHDA 二进制、包含 Apple 载荷的 SimpleHDA 构建产物、SOF 固件、HP OEM 软件、NullMoth 捆绑包、原始日志和私人品牌图标由使用者从适当来源自行取得。已有第三方源码版权说明仍适用。
