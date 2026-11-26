# Windows 高 DPI 显示

此补丁保留此前用于 Stylos 2.6.0 中文版（4 GiB 标记基线）的 DPI 清单修改，让 Windows 按桌面缩放比例放大整个界面。工具和文字会一起放大，外观可能更柔和。

保存并关闭 Stylos，在 PowerShell 中执行 `./Apply.ps1 -StylosExe "D:\RETAS\Stylos\Stylos.exe"`。安装器校验输入和输出，保留本机备份。还原时追加 `-Restore`。如果要与画布、中割、动检一起使用，请先应用 DPI 补丁，按相反顺序还原。本包仅支持脚本中列出的精确基线。
