# Better Canvas

| 组件 | 作用 | 安装包 |
|---|---|---|
| [rotation-shortcuts/](rotation-shortcuts/) | 旋转按钮、可配置快捷键、预览器登记 | `Stylos-Rotation-Shortcuts-x86.zip` |
| [zoom/mousewheel-zoom/](zoom/mousewheel-zoom/) | 指针中心滚轮，每格 5 个百分点 | `Stylos-Mousewheel-Zoom-x86.zip` |
| [zoom/anti-alias/](zoom/anti-alias/) | 环境设置中的双线性与双立方显示 | `Stylos-Better-Canvas-Display-x86.zip` |
| [fast-navigation/](fast-navigation/) | 连续旋转和缩放时降低重绘成本，停手后恢复画质 | `Stylos-Fast-Navigation-x86.zip` |

四个组件分别构建为独立插件包，在管理器中可以分别安装与启停。旋转和滚轮插件通过公开 DevKit 接收窗口消息。快速导航和画布显示同时启用时通过服务接口协调采样质量。
