# 指针中心滚轮缩放

滚轮每格改变 5 个百分点，保留高精度滚轮的剩余滚动量。缩放前记录指针下的原画坐标，缩放后再校正视口位置，支持旋转和镜像的画布。MousewheelZoom.c 实现倍率和定位，ZoomCalls.S 适配原生 x86 视图函数。

与旋转快捷键一起由 [原生输入模块的 Build.ps1](../../../stylos/tablet-native/Build.ps1) 构建。[安装包](../../../packages/Stylos-Native-Tablet-x86.zip)
