# Contributing to retas-patches

[中文](#中文) · [English](#english) · [日本語](#日本語)

## 中文

欢迎提交补丁、翻译和手册修订。贡献采用项目的 [BSD 3-Clause 许可证](LICENSE)。第一次贡献时，可以在[贡献者名单](CONTRIBUTORS.md)中加上希望展示的名字。

- 较大的改动请先开 [Issue](https://github.com/kevin-zf1123/retas-patches/issues) 讨论。报告问题时，请注明 Stylos 和插件版本、运行环境、复现步骤，以及预期与实际结果。涉及笔输入时，请写明数位板和驱动。
- 每个 PR 聚焦一项改进。若新功能需要较大的前置重构，可把重构拆成单独的 PR。请说明改动解决了什么问题、为什么选择这个实现，以及实际做了哪些验证。使用 AI 工具时，可附上有助于理解设计的提示词。
- 按 [README](README.md#开放插件系统与构建) 构建受影响的组件。修改 C/C++ 源码或头文件时，运行下方的格式化与 lint 命令。改变用户可见行为时，同步更新[使用手册](docs/manuals/README.md)和已有的相关译文；无需改文档或有待验证的环境时，在 PR 中说明。

## English

Patches, translations and manual edits are welcome under the [BSD 3-Clause License](LICENSE). With your first contribution, you can add your preferred name to the [contributors list](CONTRIBUTORS.md).

- Discuss substantial changes in an [issue](https://github.com/kevin-zf1123/retas-patches/issues) first. For a bug, include the Stylos and plugin versions, operating environment, reproduction steps, and expected and observed results. For pen input, include the tablet and driver.
- Keep each pull request focused on one improvement. Consider submitting preparatory refactoring separately. Explain the problem, the design choice and how you tested the change. If AI tools helped, you can include prompts that explain the design.
- Build the affected components using the [build instructions](README.md#开放插件系统与构建). For changed C/C++ sources and headers, run the formatting and lint commands below. Update the [manual](docs/manuals/README.md) and its existing translations when user-visible behavior changes. Mention any untested environments or why a manual update is unnecessary.

## 日本語

パッチ、翻訳、マニュアルの修正を歓迎します。寄稿には [BSD 3-Clause ライセンス](LICENSE) が適用されます。初めて寄稿するときは、[貢献者一覧](CONTRIBUTORS.md)に希望する名前を追加できます。

- 大きな変更は、先に [Issue](https://github.com/kevin-zf1123/retas-patches/issues) で相談してください。不具合の報告には、Stylos とプラグインのバージョン、使用環境、再現手順、期待した結果と実際の結果を書いてください。ペン入力の場合はタブレットとドライバーも記載してください。
- ひとつの PR にはひとつの改善をまとめてください。準備のためのリファクタリングは別の PR に分けられます。解決したい問題、実装を選んだ理由、確認した内容を書いてください。AI ツールを使った場合は、設計意図が伝わるプロンプトも添えられます。
- [ビルド手順](README.md#开放插件系统与构建)に従って、変更したコンポーネントをビルドしてください。変更した C/C++ のソースとヘッダーには、下記のコマンドを実行してください。使い方が変わる場合は[マニュアル](docs/manuals/README.md)と既存の関連訳を更新し、更新が不要な理由や未検証の環境があれば PR に記載してください。

## clang-format & cpplint

在仓库根目录运行，将示例路径换成本次修改的 C/C++ 文件。Run from the repository root with the paths you changed. リポジトリのルートで、変更したファイルのパスを指定してください。

```sh
python3 -m pip install "clang-format==21.*" cpplint
clang-format -i stylos/motion-check/worker/main.cpp
python3 -m cpplint stylos/motion-check/worker/main.cpp
```
