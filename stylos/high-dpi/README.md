# Windows 高 DPI 显示

此补丁保留此前用于 Stylos 2.6.0 中文版（4 GiB 标记基线）的 DPI 清单修改，让 Windows 按桌面缩放比例放大整个界面。工具和文字会一起放大，外观可能更柔和。

用管理器导入 Stylos-High-DPI-x86.zip，再应用更改。管理器按 plugin.json 声明的资源 RVA 定位清单，支持与动检、画布和中割组合启停。此项需要受支持的中文 4 GiB 基线。
