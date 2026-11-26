# Stylos 插件管理器

解压后双击 StylosPluginManager.exe，选择本机的 Stylos.exe。首次安装前请保存工作并退出 Stylos。管理器随后可从 **编辑 → 设置 → 插件管理器…** 打开；同级还有原有的“环境设置…”。默认快捷键为 Ctrl+Shift+P（P 被 Stylos 占用时尝试 J、M），可在原生“快捷键”设置中修改。

**从安装包安装…** 支持本次发行注册表中的组件 ZIP。先解压最外层合集，可一次选取多个子包。列表显示已安装、启用及待应用的变化。停用保留安装记录，可重新启用；卸载清除组件的安装记录和启用状态。原生 WinTab 和 Windows Ink 互斥。旋转和滚轮作为两个插件分别管理。

**应用更改** 前会校验本机版本、冲突和相关文件。Stylos 正在运行时，管理器会等待这台 Stylos 正常退出再执行；可“取消等待”。画稿不会被强制关闭。**打开设置** 跳转到该插件的设置页。用户的比例和画布选项在停用、卸载后保留。

注册表在 registry.json 中维护插件 ID、状态位、设置入口和冲突；发布时绑定子包 SHA-256。未知 ZIP 和构建不会被执行。文件变更记录和原件备份位于安装目录的 .stylos-plugins。出现中断时，先关闭 Stylos，再打开管理器恢复。请保留这个目录。

## 版本、更新与备份

本版管理器和动检组件为 `0.1.1`，其余七项为 `0.1.0`。更新管理器后，导入新版动检 ZIP 并应用即可启用启动修复。版本列使用组件清单和安装记录。历史包也可按已知摘要识别，重新打包的组件可按清单导入。上一版地址映射管理器的八个发行包登记为 `v0`。从旧安装迁移时，管理器会核对已有安装记录、管理器校验记录和当前宿主的地址表，再为其中记录的组件标记 `v0`。更早的受支持补丁以及缺少版本依据的安装显示“无注册版本”。

**从包更新管理器**和**从安装包安装**都接受单个新版管理器 ZIP。**在线检查更新**从本项目 GitHub 最新 Release 获取同名文件。两种更新都使用包内的产品标识和版本号选择适用的新版。首次从没有更新入口的旧管理器升级，仍需解压新版管理器到单独目录，运行后选择原 Stylos.exe。之后可以在管理器内更新。更新管理器会保留原有组件版本和启停状态。

**备份全部插件**导出已安装的组件 ZIP、版本记录和启停状态，包括停用项。右键组件可以单独备份。**恢复备份**会先核验整包，再事务性应用。全量恢复替换插件列表，单项恢复合并到当前列表，并按原规则处理互斥插件。对于安装前就已存在且找不到原 ZIP 的老插件，备份使用可重建该组件的登记包，并保留“无注册版本”标记。用户全局的快捷键和绘图偏好不随 ZIP 迁移。

**卸载管理器**会自动导出插件备份到安装目录的 `.stylos-plugins/backups/plugins-*.zip`，恢复对应的 Stylos 基线并移除受管组件和管理器。外部修改的同名组件文件保留。本版安装时新增的管理器快捷键会在仍匹配时移除，已有快捷键和个人设置保留。在正在运行的管理器中更新或卸载时，当前管理器退出后由其已校验的本地副本完成文件替换，结果写入 `.stylos-plugins/maintenance-result.txt`。

## 命令行

```powershell
.\StylosPluginManager.exe --setup D:\RETAS\Stylos
.\StylosPluginManager.exe --list D:\RETAS\Stylos
.\StylosPluginManager.exe --install D:\RETAS\Stylos D:\patches\Stylos-Mousewheel-Zoom-x86.zip
.\StylosPluginManager.exe --disable D:\RETAS\Stylos mousewheel-zoom
.\StylosPluginManager.exe --enable D:\RETAS\Stylos mousewheel-zoom
.\StylosPluginManager.exe --uninstall D:\RETAS\Stylos mousewheel-zoom
.\StylosPluginManager.exe --update D:\RETAS\Stylos D:\patches\Stylos-Plugin-Manager-x86.zip
.\StylosPluginManager.exe --update D:\RETAS\Stylos
.\StylosPluginManager.exe --backup D:\RETAS\Stylos D:\backup\all.zip
.\StylosPluginManager.exe --backup D:\RETAS\Stylos D:\backup\wheel.zip mousewheel-zoom
.\StylosPluginManager.exe --restore D:\RETAS\Stylos D:\backup\all.zip
.\StylosPluginManager.exe --self-uninstall D:\RETAS\Stylos
```

运行 build_host_packages.py，使用 --host 多次传入对应的本地参考基线、--inbetween 传入原 v0.1 安装器、--out 指定输出目录。完整发行需要 versions.json 的各 base、stage1、main 镜像。该命令链接参考镜像，并生成管理器和组件 ZIP。可用 --manager-assets 保存中间资源，之后使用 build_manager.py --assets 和 --out 重新打包。组件 ZIP 中的 plugin.json 与构建时的地址声明须一致。

应用后，.stylos-plugins/layout.json 按 Stylos.exe 和各 worker 列出实际分配。该文件与安装状态在同一事务中写入或恢复。地址格式、固定入口和新补丁节的声明见 [DevKit 文档](../../retas-devkit/README.md)。

verify_layout.ps1 使用 32 位 Windows PowerShell，传入解压的 -Manager、组件目录 -Packages 和原版文件数组 -Baseline。脚本在临时目录内重建目标并验证哈希、地址冲突、动检启停时的中割重定位、重复应用和卸载。可用 -LegacyPackages 验证上一版组件。


## 登记发行版本

插件源码目录的 `plugin.json` 与管理器注册表中的 `version` 应一致。为新版 ZIP 在 `registeredVersions` 中追加版本和 SHA-256，历史登记保留。构建器会登记当前产物的摘要，并为历史包保留版本索引。地址布局与插件发行版本分别校验。

管理器 ZIP 包含 `update.json` 和 `StylosPluginManager.exe`，可直接构建并分发。在线更新读取最新 GitHub Release 中名为 `Stylos-Plugin-Manager-x86.zip` 的附件。
