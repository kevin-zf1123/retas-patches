# RETAS DevKit · Stylos 2.6.0 x86

DevKit 把已验证的宿主入口转换成有版本的接口，供其他本机工具使用。插件包的发现、安装、卸载和启停属于 [插件管理器](../plugin-manager/stylos/)。本版适配器作为 StylosCanvasFilter.dll 的一部分发布。

**入口注册表**见 entries.json。ABI v1 通过同一 Windows 桌面上的注册消息 RETAS.Stylos.DevKit.v1 通信，目标为 DrawFrameWnd。wParam 为固定入口 ID，lParam 为该入口定义的整数参数。已有 ID 的语义保持稳定。增加入口须同步注册表、retas.h 和 StylosApi.c；不兼容的变更使用新的 ABI 版本和消息名。

Python 客户端只依赖标准库；也可从 C/C++ 包含 retas.h 并使用 SendMessageTimeoutW。先查询版本和能力；请求设置页或管理器前，请确保对应插件已启用且主窗口可交互。可指定窗口句柄以选择特定实例。

```powershell
python retas-devkit/retas_client.py version
python retas-devkit/retas_client.py zoom
python retas-devkit/retas_client.py preferences --page midpoint
python retas-devkit/retas_client.py preferences --page motion
python retas-devkit/retas_client.py manager
```

**宿主适配**见 stylos260.py。固定版本的绘制、设置对话框初始化、主消息循环接入点在此校验和链接。运行期查询在宿主 UI 线程处理，不跨进程传递指针。管理器从公开接口请求切换设置页；包哈希、状态与冲突由管理器的 registry.json 和每安装目录的状态文件维护。

## 离线补丁地址接口

管理器在 Stylos 关闭后为每个目标 EXE 单独建立 `PatchMap`。接口用于安装、启停和卸载阶段。运行中的 Stylos 继续使用上面的消息 ABI v1。[C# 接口](PatchMap.cs) · [构建端校验](patch_layout.py)

| 接口 | 地址语义 |
|---|---|
| `Reserve(owner, rva, size)` | 声明宿主中已有的半开区间。RVA 从镜像基址起算，管理器通过目标 PE 的节表换算文件偏移，两个所有者的区间重叠会报错。 |
| `Allocate(owner, name, size)` | 分配命名补丁节。节内逻辑地址从 0 开始，管理器按目标 PE 的 SectionAlignment 和 FileAlignment 分别对齐 RVA 和文件位置。也可分别传入 virtualSize 和 rawSize。 |
| `Resolve(owner, region, address, size)` | 查询本插件声明范围内的一段地址，返回实际 RVA、文件偏移和长度。原有入口使用区域名 `host`，新节使用声明的节名。 |

每个插件在源码目录的 `plugin.json` 中通过 `sites` 声明固定 RVA 和字节长度，通过 `segments` 声明新节。动检另外使用 `workerSites` 描述 worker 自身的入口。通过 DLL、输入配置或共享消息入口工作的插件可以声明空数组。管理器自己的消息入口、共享设置入口和导入节也有同样的声明。[管理器声明](../plugin-manager/stylos/plugin.json) · [动检声明](../stylos/motion-check/plugin.json)

以中割为例，插件申请 `.ltalign`，节内地址从 0 开始。它在 `fixups` 中声明两个宿主调用点和 18 个宿主引用。每个修正项指定所在区域、逻辑地址、参考节、连续 32 位字段个数和方向。写入值等于规范化字段值加上 `factor × 参考节实际 RVA`。动检启停会改变中割节的位置，同一份中割代码可按新位置链接。[中割声明](../stylos/rotation-scale-inbetween/plugin.json)

添加或更改补丁时，将清单登记在管理器注册表，在 `sectionOrder` 中指定新增节的分配次序，并为支持的基线生成参考镜像。构建器校验每次写入都有声明、固定区间无冲突、分配位置与参考链接匹配。安装时管理器按启用状态选取各插件的片段，从已识别的基线重新分配和绑定，并在写盘前核对最终 SHA-256。旧安装和历史变体可通过已有差分恢复到对应基线。[注册表](../plugin-manager/stylos/registry.json)

应用后的 `.stylos-plugins/layout.json` 按镜像和插件记录分配。每行的 `owner`、`region`、`address` 标识逻辑地址，`rva`、`fileOffset`、`size` 给出本次落点和长度，单位均为字节。查询时应核对文件中的 `host` 与当前 Stylos.exe 的 SHA-256。
