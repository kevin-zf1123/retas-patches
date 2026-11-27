# RETAS DevKit 0.2

使用本 SDK 可以独立开发 Stylos 2.6.0 x86 插件。交付物是一个 ZIP，最终用户在 **Stylos 插件管理器 → 从安装包安装…** 选择它并应用。管理器启动时列出本机实际安装的包，安装新 ID 和替换同 ID 的包都使用这个入口。

## 从示例构建一个包

在 Windows 上解压 SDK，将 Zig 0.13 或兼容的 x86 C 编译器加入 PATH。以下 PowerShell 命令在 SDK 根目录运行。可以更改示例清单的 `id`、`name` 和 `version`，然后单独分发输出的 ZIP。

```powershell
New-Item -ItemType Directory payload
Copy-Item examples/view-info/plugin.json payload/plugin.json
zig cc -target x86-windows-gnu -O2 -shared -s -I. examples/view-info/view-info.c module.def -o ViewInfo.dll -luser32
Copy-Item ViewInfo.dll payload/ViewInfo.dll
python pack.py payload ViewInfo.zip
```

打开管理器，导入 `ViewInfo.zip` 并点击“应用更改”。启动 Stylos，在“环境设置 → 插件 → 当前视图”可看到示例 DLL 从宿主查询的倍率和角度。管理器里的“打开设置”会直接定位该页。停用或卸载也通过管理器执行；应用文件更改前需要退出 Stylos。[示例源码](examples/view-info/view-info.c)

## 包清单

```json
{
  "schema": 3,
  "host": "stylos-2.6.0-x86",
  "id": "sample-view-info",
  "name": "视图信息示例",
  "version": "1.0.0",
  "module": "ViewInfo.dll",
  "settings": "当前视图"
}
```

- `id` 以小写字母开头，后续可用小写字母、数字和连字符，最多 64 字符。版本格式为 `major.minor.patch`。同 ID 包更新实现和清单，备份保持原包字节。
- `module` 是包根目录的 x86 DLL。模块安装到该 ID 的私有目录，通过进程内 ABI 提供功能。模块、原生文件或地址布局均可作为包的实现。
- `settings` 是环境设置页名称。宿主在 UI 线程上调用 `create_page`，插件返回初始隐藏的子窗口。点击外层“确定”时调用 `accept_page`；取消时直接销毁页，插件可把未提交值保存在控件里。
- `order` 决定应用布局及分发回调的顺序，默认 0；相同顺序按 ID 排列。`conflicts` 是互斥 ID 数组，`exclusive` 是互斥组名。启用组内某一插件时，管理器停用同组的其他插件。
- 特定宿主需要从安装根目录加载的文件可以用 `"files":["Example.dll"]` 声明，源文件放在 ZIP 的 `files/Example.dll`。文件名冲突或已有外部文件会在应用前报告。
- 私有资源和依赖 DLL 使用 `assets` 列出相对路径，例如 `["图标/工具.png","MyDependency.dll"]`。将它们放在 ZIP 的 `payload/` 下，管理器会按目录结构部署到模块旁边，并在停用或卸载时撤下。
- 可选 `shortcuts` 数组声明原生命令及默认绑定，例如 `{"command":"EXAMPLE_ACTION","modifiers":3,"candidates":"PJM"}`。位 1、2、4 分别表示 Ctrl、Shift、Alt。插件负责注册及处理命令。

## 进程内扩展

DLL 通过未修饰导出名 `RetasPluginInit` 提供 x86 stdcall 入口。宿主在第一次进入主消息循环时加载启用模块，传入带大小、版本的接口表。返回 FALSE 会放弃本次初始化。模块的窗口回调和服务指针在本次 Stylos 进程中有效，启停在下次启动时生效。[ABI 头文件](plugin.h)

- `before_message`、`message` 分别在读取消息前和取到消息后调用；处理了输入的插件可把消息设为 `WM_NULL`。
- `canvas_tile` 在经验证的缩放或旋转绘制入口完成原生绘制后调用一次。参数是 Stylos 2.6 的视图、目标位图、源、缓冲区和更新矩形，仅在回调期间有效。
- `provide` 和 `service` 允许模块发布、查询带版本的服务名，具体接口由提供方定义。可在消息或绘制回调中查询服务，此时其他模块已完成初始化。
- 外部工具仍可使用注册窗口消息 `RETAS.Stylos.DevKit.v1` 查询视图。管理器按插件 ID 定位设置页，参数为 `RegisterWindowMessageW(L"RETAS.Stylos.Settings.<id>")`；旧版数值别名保留给相应页面。[消息入口](entries.json)

## 声明地址与所有权

需要修改宿主镜像的包可包含 `layout.json`。单纯使用模块回调的插件可以省略它。管理器从校验过的基线重建启用组合，并在写盘前核对位置和所有权。下面是格式示意；地址和预期字节应由插件作者在支持的宿主版本中验证。

```json
{
  "edits": [{"rva": 123456, "expected": "data/original.bin", "data": "data/replacement.bin"}],
  "sections": [{"name": ".sample", "virtualSize": 4096, "flags": 1073741888, "data": "data/section.bin"}],
  "relocations": [{"region": ".sample", "address": 0, "target": ".sample", "addend": 16}]
}
```

固定修改需要等长的预期字节和替换数据。新节由管理器分配，重定位把本插件目标区域的 RVA 写入本插件拥有的四字节位置。`host` 使用宿主 RVA，其他区域使用节内偏移。重复区域、未声明位置、前置字节不匹配都会终止本次应用。[地址分配器](PatchMap.cs)

管理器 0.2.2 支持 x86 的四字节相对位移。把重定位中的 `target` 换成 `relativeTarget`，管理器将写入从该字段末端到目标地址的有符号位移。源字段和目标字节都须属于同一插件。

需要维护旧版链接镜像的包还可提供 `variants`。它按当前主镜像的 SHA-256 选择差分，`images` 中的 `Stylos.exe` 基于当前主镜像，工作进程基于原始主程序。插件用 `claims`、`workerClaims`、`ownedSections` 标出其占用。管理器校验差分输入和输出摘要。安装记录与实际分配写入安装目录的 `.stylos-plugins`，卸载一个插件时从基线重建剩余组合。
