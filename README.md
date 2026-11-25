# retas-patches

**Stylos 旋转缩放中割对位 · Rotation and scale inbetween alignment · Stylos 回転・拡縮を伴う中割りの位置合わせ**

[下载 v0.1 / Download v0.1 / v0.1 をダウンロード](https://github.com/kevin-zf1123/retas-patches/releases/download/v0.1/Stylos-Inbetween-x86.zip)

## 中文

这个补丁扩展了 32 位 Windows 版 RETAS STUDIO Stylos 2.6.0 的中割对位功能。两张原画在透光台上对齐后，补丁会按指定中割比分配它们之间的旋转、缩放和平移，让两张原画在中割位置重合，当前中割画纸保持原位。可以沿用 Stylos 原有工具手动对齐，也可以在两张原画上各点两个对应特征，快速完成对齐。

关闭 Stylos，先备份本机的 `Stylos.exe`。解压下载的 ZIP，运行 `StylosInbetween.exe`，选中原安装目录的主程序。安装后继续用原来的快捷方式启动。需要 .NET Framework 4.5 或更高版本。[中文使用与构建说明](stylos/rotation-scale-inbetween/README.zh-CN.md)

## English

This patch extends inbetween alignment in RETAS STUDIO Stylos 2.6.0 for 32-bit Windows. Once the two key drawings are aligned on the light table, the patch distributes their relative rotation, scale and translation according to the chosen inbetween fraction. Both references meet at the inbetween position while the active drawing stays in place. Align the references manually with the existing light-table tools, or use the four-point shortcut by picking the same two features on each drawing.

Close Stylos and back up your installed `Stylos.exe`. Extract the ZIP, run `StylosInbetween.exe`, and select that executable. You can keep using your existing launcher. Installation requires .NET Framework 4.5 or later. The patch UI is in Simplified Chinese. [English usage and build guide](stylos/rotation-scale-inbetween/README.md)

## 日本語

32 ビット Windows 版 RETAS STUDIO Stylos 2.6.0 の中割りの位置合わせを拡張するパッチです。ライトテーブルで前後の原画を合わせておくと、指定した中割りの比率に応じて回転・拡縮・移動を両方の原画に振り分けます。動画用紙を動かさずに、中割りの位置で前後の原画を重ねられます。原画同士は従来のツールで手動で合わせることも、対応する特徴を各原画で 2 点ずつ指定してすばやく合わせることもできます。

Stylos を終了し、使用中の `Stylos.exe` をバックアップしてください。ZIP を展開して `StylosInbetween.exe` を起動し、インストール先の `Stylos.exe` を選びます。適用後も従来のショートカットを使えます。.NET Framework 4.5 以降が必要です。パッチの画面表示は簡体字中国語です。[日本語の使い方とビルド手順](stylos/rotation-scale-inbetween/README.ja.md)

## 许可证 / License / ライセンス

[BSD 3-Clause](LICENSE) · 补丁源码与安装器 / Patch source and installer / パッチのソースコードとインストーラ
