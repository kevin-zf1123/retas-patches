# RETAS DevKit · Stylos 2.6.0 x86

DevKit 把已验证的宿主入口转换成有版本的接口，供其他本机工具使用。插件包的发现、安装、卸载和启停属于 [插件管理器](../plugin-manager/stylos/)。本版适配器作为 StylosCanvasFilter.dll 的一部分发布。

**入口注册表**见 entries.json。ABI v1 通过同一 Windows 桌面上的注册消息 RETAS.Stylos.DevKit.v1 通信，目标为 DrawFrameWnd。wParam 为固定入口 ID，lParam 为该入口定义的整数参数。已有 ID 的语义保持稳定。增加入口须同步注册表、retas.h 和 StylosApi.c；不兼容的变更使用新的 ABI 版本和消息名。

Python 客户端只依赖标准库；也可从 C/C++ 包含 retas.h 并使用 SendMessageTimeoutW。先查询版本和能力；请求设置页或管理器前，请确保对应插件已启用且主窗口可交互。可指定窗口句柄以选择特定实例。

```powershell
python retas-devkit/retas_client.py version
python retas-devkit/retas_client.py zoom
python retas-devkit/retas_client.py preferences --page midpoint
python retas-devkit/retas_client.py manager
```

**宿主适配**见 stylos260.py。固定版本的绘制、设置对话框初始化、主消息循环接入点在此校验和链接。运行期查询在宿主 UI 线程处理，不跨进程传递指针。管理器从公开接口请求切换设置页；包哈希、状态与冲突由管理器的 registry.json 和每安装目录的状态文件维护。
