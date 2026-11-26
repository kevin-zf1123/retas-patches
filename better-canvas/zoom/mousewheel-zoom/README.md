# 指针中心滚轮缩放

从插件管理器安装 Stylos-Mousewheel-Zoom-x86.zip，可单独启用或停用。滚动一格改变 5 个百分点，缩放围绕指针下的画稿坐标，支持旋转、镜像和高精度滚轮。

MousewheelZoom.c 实现缩放和定位，ZoomCalls.S 适配 Stylos x86 视图函数，共享宿主消息入口由 Better Canvas 提供。旧的原生数位板组件仍可独立使用。
