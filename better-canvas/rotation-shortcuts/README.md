# 画布旋转与快捷键

工具栏按钮和默认的 `-`、`=` 键让视图每次转动 5°。Rotation.c 处理视图和工具栏，RotationCalls.S 适配原生 x86 调用约定。StylosShortcuts.c 负责在 Stylos 注册和派发快捷键，包括文件预览器的“登记到透光台”（新配置 Ctrl+Shift+L）。修改键位后沿用当前用户设置。

与滚轮缩放一起由 [原生输入模块的 Build.ps1](../../stylos/tablet-native/Build.ps1) 构建。[安装包](../../packages/Stylos-Native-Tablet-x86.zip)
