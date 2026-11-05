# NeoGUI

NeoGUI edits the binary `.gui` layouts used by Star Wars: Knights of the Old
Republic and Knights of the Old Republic II.

Support is offered in the OpenKotOR Discord.

[![OpenKotOR Discord](https://discordapp.com/api/guilds/739590575359262792/widget.png?style=banner2)](https://discord.gg/OpenKotOR)

## Editing layouts

- Open and save native `.gui` files directly.
- Select controls in the tree or preview, then drag to move or resize them.
- Enter exact geometry or snap movement to a 1, 4, 8, or 16 pixel grid.
- Edit native GFF fields, with undo and redo for layout changes.
- Preview textures, bitmap fonts, borders, sliders, scrollbars, and progress bars.
- Switch between normal, highlighted, selected, and highlighted-selected states.
- Select K1 or K2 preview rules and archive or loose/Override layout scaling.
- Load a talk table to resolve localized string references.

Open a layout with **File → Open GUI**. Use **File → Asset folder** to select a
folder containing its extracted textures and fonts, then enable **Game assets**.
Load `dialog.tlk` with **File → Load talk table** for string references. When
hosted in NeoToolKit, **File → Use game-browser assets** uses the host's resource
resolver instead of an extracted asset folder.

Click a control to select it, drag its lower-right handle to resize it, or enter
values in the geometry fields and choose **Apply geometry**. Double-click a
field in the inspector to edit its value. **Show all layout fields** exposes
the whole document. Save the edited layout to your working or mod directory.

## GitHub Actions builds

The **Build** workflow runs on pushes, pull requests, manual dispatch, and
`neoshared-updated` repository dispatches. It checks out NeoShared's `main`,
builds Release packages, and keeps the downloadable artifacts for 14 days.

| Platform | Runner | Package |
| --- | --- | --- |
| Linux x86-64 | Ubuntu 24.04 | `.tar.gz` containing `bin/NeoGUI` |
| Windows x86-64 | Visual Studio 2026 | `.zip` containing `bin/NeoGUI.exe` |
| macOS Apple Silicon | macOS 15 | `.zip` containing `NeoGUI.app` |
| macOS Intel | macOS 15 Intel | `.zip` containing `NeoGUI.app` |

Windows uses NeoShared's wxWidgets 3.3.3 overlay with static libraries. macOS
packages include their non-system library dependencies through NeoShared's
bundle packager. Linux packages use the system wxGTK 3.2 runtime.

## Building from source

Requirements are a C++17 compiler, CMake 3.16 or newer, wxWidgets, zlib, and
[NeoShared](https://github.com/vrifftech/neoshared). Place the source directories
beside one another as `NeoGUI/` and `neoshared/`, or pass another location with
`-DNEOSHARED_ROOT=/path/to/neoshared`.

NeoShared must include `neogff::GffFile::SaveBytes`, which supplies the editor's
in-memory undo snapshots. Publish the accompanying NeoShared serialization
update before running CI against NeoShared's `main` branch.

### Linux

On Ubuntu 24.04, install the dependencies and run these commands from `NeoGUI`:

```sh
sudo apt-get install build-essential cmake ninja-build pkg-config zlib1g-dev libwxgtk3.2-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DNEO_MINIMAL_RELEASE=ON
cmake --build build --parallel 2
./build/NeoGUI
```

### macOS

```sh
brew install cmake ninja wxwidgets
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DwxWidgets_CONFIG_EXECUTABLE="$(brew --prefix wxwidgets)/bin/wx-config" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DNEO_MINIMAL_RELEASE=ON
cmake --build build --parallel 2
open build/NeoGUI.app
```

The local build uses the current machine's architecture. CI builds Intel and
Apple Silicon packages separately.

### Windows

Use Visual Studio 2026 with the Desktop development with C++ workload, CMake,
and a vcpkg checkout. Set `VCPKG_ROOT` to that checkout, then run PowerShell from
`NeoGUI`:

```powershell
$shared = (Resolve-Path ../neoshared).Path
& "$shared/scripts/install-wxwidgets.ps1" -VcpkgRoot $env:VCPKG_ROOT -Triplet x64-windows-static
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 `
  "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  "-DVCPKG_OVERLAY_PORTS=$shared/vcpkg-ports" `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static
cmake --build build --config Release --parallel 2
./build/Release/NeoGUI.exe
```
`neogui::wx` and create the editor through `neogui::ui::createEditorPanel`.

Both build options default to `ON`. The standalone application installs with
`cmake --install build --config Release --prefix stage`.
