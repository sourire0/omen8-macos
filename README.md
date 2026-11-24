# HP OMEN 8 macOS 移植与修复

面向 HP OMEN 16-b1xxx 暗影精灵 8 的工程记录，实机为 Core i5-12500H、Iris Xe、RTX 3060 Laptop、1920×1080 内屏，验证系统为 macOS Tahoe 26.7.x x86_64。其他 BIOS、子型号、GPU、屏幕和 macOS 构建需重新验证。Apple 的 macOS 软件许可有硬件适用限制。 [Apple 软件许可](https://www.apple.com/legal/sla/)

- [从 Windows 制作启动盘到验收](docs/install-from-windows.md)
- [修复过程、架构与实测结果](docs/engineering.md)
- [源码构建与交付清单](docs/build.md)
- [EFI 配置参考片段](efi/OMEN8-reference.plist)

交付包含 Xe 面板补丁、Intel DMIC 内核桥与 CoreAudio HAL 源码、SimpleHDA 集成差异、OMEN MUX 应用及驱动源码。启动链和上游依赖请按各项目发布说明组装。EFI 参考片段需合并到同版 OpenCore 的 Sample.plist 并校验。
