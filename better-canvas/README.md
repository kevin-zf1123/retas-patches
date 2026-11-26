# Better Canvas · Stylos 2.6.0

| 组件 | 功能 | 安装包 |
|---|---|---|
| [rotation-shortcuts](rotation-shortcuts/) | 画布工具栏左右旋转 5°、可绑定快捷键 | [视图控制与原生 WinTab](../packages/Stylos-Native-Tablet-x86.zip) |
| [zoom/mousewheel-zoom](zoom/mousewheel-zoom/) | 以指针为中心，每格增减 5 个百分点 | 同上 |
| [zoom/anti-alias](zoom/anti-alias/) | 优化的双线性、双立方显示及环境设置 | [画布插值](../packages/Stylos-Better-Canvas-Display-x86.zip) |

CanvasInput.c 处理 Stylos UI 线程的消息并把操作分发给对应组件，CanvasView.h 定义共享视图接口。滚轮和旋转沿用已验证的输入模块入口并一起编译；插值独立安装，可与两种数位板方案配合。
