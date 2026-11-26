# 原生数位板、滚轮与视图快捷键

Windows x86 Stylos 2.6.0。绘王使用系统的 32 位 WinTab，Wacom 启动脚本选择厂商的 Wacom_Tablet.dll。这个组件针对双驱动共存和非 100% 桌面缩放时笔尖落点偏离的情况。需要相应厂商驱动。

保存并退出 Stylos，备份安装目录内已有的同名文件。将本包 wintab32.dll 与 Stylos-Wacom.cmd 放在 Stylos.exe 旁。绘王照常启动，Wacom 双击该 cmd。撤销时移走这两项并恢复原来的同名文件。Windows Ink 方案和此方案二选一。

滚轮以鼠标指针为中心，每格改变 5 个百分点。工具栏支持左右旋转 5°。快捷键注册中可以重新绑定 `-`、`=` 和文件预览器“登记到透光台”（新配置默认 Ctrl+Shift+L）。已有配置沿用原绑定；若旧版本留下 Ctrl+Alt+L，可在快捷键设置中改为 Ctrl+Shift+L。预览器支持一次选多张。

构建示例：`./Build.ps1 -Tcc C:\tools\tcc\tcc.exe`（32 位 TinyCC 0.9.27）。独立的画布显示补丁负责缩放画质。
