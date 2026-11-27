# Stylos inbetween alignment for Windows x86

The current installer embeds the editor in Preferences → Plugins → Inbetween alignment. References below to the footer button describe the original v0.1 release.

[中文](README.zh-CN.md) · [English](README.md) · [日本語](README.ja.md)

The patch extends **Light Table → Alignment → Move image to middle position**. Once two key drawings are aligned, it distributes their relative rotation, scale and translation according to the chosen inbetween fraction. Both references meet at the inbetween position while the active drawing stays in place. Use two unmirrored 2D references. You can align them manually with the light-table tools or speed up alignment by picking the same two features on each drawing.

## Install and use

1. Close Stylos and back up the **current local Stylos.exe**, especially on a workstation with custom patches. If this is the first launch, open and close Stylos once to create its shortcut profile.
2. Extract the package, run **StylosInbetween.exe**, select the installed **Stylos.exe** and confirm the in-place update. Launch Stylos from the same pinned taskbar or Start menu shortcut. Each workstation uses its own executable and plugins. Installation requires .NET Framework 4.5 or later and write access to the installation folder.
3. Open the middle drawing. Register the start and end drawings on the light table in that order. For manual alignment, align corresponding features using the native light-table controls. In sets with more than two entries, select exactly the two references.
4. Press the shortcut shown by the installer (default **Ctrl+Alt+Shift+I**). Click **1/2**, **2/3** or **1/3** to align immediately. On first use, Enter selects **1/2**. The first row is the start, and the second row is the end.
5. Click **▼** to enter another fraction, such as **2/5**, or select a registered ratio from the editable list. **应用** (Apply) aligns the references. Escape closes the panel. Numerators may range from zero to the denominator, and denominators from 1 to 10000.

### Pick four features on the canvas

Put the two sheets in start-to-end order and activate their light-table set on the current drawing. Pan or zoom before entering the command so both landmarks fit on the canvas. Press the alignment shortcut, select **▼**, enter the desired fraction (initially 1/2), and click **四点自动对位…**.

1. Click two distinct features on the start reference. Widely separated features reduce the effect of pointing error on the measured angle.
2. The canvas switches to the end reference. Click its two corresponding features in the same order. Releasing the fourth click opens the alignment preview at the chosen progress.
3. Right-click or select **上一步** to undo a point. **移动提示窗** moves the instructions away from a feature. Escape or Cancel restores the original reference visibility and leaves both transforms intact.

In the **对位预览** panel, select the start or end reference by its light-table title. Both drawings are visible. Two solid red dots mark the selected reference’s features directly on the canvas. Each dot is centred on its draggable landmark. Hold and drag a solid dot to correct that feature. Both references rotate, scale and translate continuously at the original progress. During the drag, two pale red dots show the feature being corrected and its counterpart on the other sheet, both in their mouse-down poses. The two solid dots show the current alignment. Releasing hides the pale guides and keeps the latest fit. The other three source features retain their locations. You can change layers and make further corrections.

Double-click the canvas, press Enter or click **确定** to keep the result. Escape or **取消** restores both references to their transforms before four-point picking. Enable **四点对位后跳过预览** in the ratio preferences to apply immediately on the fourth click.

The picker uses the native view-to-paper conversion and accounts for existing transforms on each reference. It shows one reference at a time and restores the previous visibility and opacity after picking. Picking and preview use the same floating-point reference transforms.

In Preferences, enable **每次对位前点选四个特征点** to start four-point picking with any quick ratio. Combine it with **直接执行 1/2，不显示比例窗口** to go straight from the shortcut to picking four points at half progress.

### Register ratios in preferences

Choose **Edit → Settings → Preferences** (编辑 → 设置 → 环境设置) and click **注册中割比…** at the bottom left. Edit the fraction next to the list, then add it, change the selected entry or delete the entry. **恢复默认** restores the three factory ratios. Entries are displayed in registration order. The first three appear as quick buttons, and the expanded list includes all entries. Equivalent fractions share one entry.

Select **直接执行 1/2，不显示比例窗口** to apply a midpoint immediately whenever the alignment command runs. Clear it in the same preferences dialog to restore the chooser. **锁定两张参考图的精确倍率比** controls scale-ratio locking for both modes. Confirm both the ratio editor and the outer Preferences dialog to save the changes for the current Windows user. Cancelling Preferences discards the pending changes.

The installer adds the shortcut to the user's native `shortcut.rac`. If I is occupied, it tries J, K, U and O with the same modifiers. The original midpoint command remains editable in Stylos shortcut settings. To revert the executable, close Stylos and restore the local backup.

## Scale and rotation

For relative similarity T from row A to row B and progress t, the requested row transforms are T^t and T^(t−1). Fractional scaling uses k^t. Angles follow the shortest rotation, retaining the input sign at exactly 180 degrees. Coordinates use each sheet's centre and a common physical unit. Absolute document pixels are never resampled by the command.

On the supported Stylos 2.6 Chinese imaging library, both references retain fractional angles and scale percentages in memory. Translation uses double-precision centimetres. A one-degree relative rotation at half progress displays at −0.5° and +0.5°. Native numeric controls and saved light-table settings still use whole degrees and percentages. Reloading those settings restores the integer values.

Other imaging library builds use the integer display path. On that path, **锁定两张参考图的精确倍率比** selects integer percentages that preserve the original scale ratio.

## Compatibility and build

The installer resolves the registered midpoint command through both dispatch tables and verifies the two-row selection, setters, imports, refresh and return path. It prepares the updated executable in the installation directory and atomically replaces Stylos.exe. Running it again recognises the installed version and can repair the shortcut binding. Its supported layout is the unsigned, non-relocatable PE32 Stylos 2.6 implementation. A changed command implementation, incompatible image layout or unrecognised installed patch produces an error. The installer also upgrades the previous English fraction-dialog, Chinese quick-ratio, four-point and crosshair-preview releases after verifying their command code, bindings and entry points. The installer preserves other shortcuts and compatible local executable edits.

Build from the repository root with `python3 stylos/rotation-scale-inbetween/build.py dist/stylos/rotation-scale-inbetween`. The build uses Python 3, LIEF, Zig 0.13 and either .NET Framework csc or Mono mcs. The deployed interpolation code uses scalar x86 and x87. The separate `check.c` validation program compares it to a spiral-centre reference and times the interpolation of both rows with Windows thread cycle counters.
