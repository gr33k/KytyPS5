# KytyPS5 (fork)

> **Fork note:** this repository is a fork of [KytyPS5/KytyPS5](https://github.com/KytyPS5/KytyPS5)
> (`upstream`). It tracks upstream `main` and adds the compatibility,
> diagnostics, and launcher changes listed below. Game compatibility results
> in this file were measured on the test platform described under
> [Test Platform](#test-platform) and apply to this fork only.

## Changes in This Fork

Relative to upstream `main`:

- **Skip gracefully instead of aborting.** Where upstream hits an
  unrecoverable path, this fork keeps the game running where it is safe to do
  so: draws/dispatches with unmaterializable shader resources, failed pipeline
  creations, unresolvable texture overlaps (equal-address images), and
  unrepresentable stencil state are skipped with a log message.
- **Per-game status and notes in the launcher.** Statuses and comments can be
  set per title ID and are merged on save, so local notes persist across
  remote compatibility-database refreshes instead of being overwritten.
- **File-driven compute-shader skip list.** A `skip_cs_hashes.txt` file next
  to the executable lists shader hashes (one per line, or separated by
  spaces/commas) whose dispatches are skipped. Used to work around
  known-hanging compute kernels during triage.
- **Identifiable builds.** The window title and logs include the short git
  hash of the build, so test results can be tied to an exact commit.
- **AMD / RX580-class compatibility.** Enables primitive-topology list restart
  via `VK_EXT_primitive_topology_list_restart` where supported, and falls back
  to `General` image layout when a depth feedback loop is unsupported.
  Strips/fans are expanded for barycentric draws on AMD.
- **Shader decoder additions.** Implements VOPC `0xbb` (`V_CMPX_LE_U16`) and
  accepts clamp/output-modifier on `V_MIN3`/`V_MAX3`/`V_MED3_F16`, with
  regression tests.
- **Triage logging.** Per-dispatch shader hashes, indirect-dispatch counts,
  subgroup caps, VMA heap budgets, and GPU-assisted validation options to
  diagnose hangs and device-memory pressure.

Upstream merges are pulled in regularly; local commits sit on top.

## About KytyPS5

KytyPS5 is a free and open-source PlayStation 5 emulator written in C++ for
Windows and Linux, with experimental macOS support. It is based on a heavily
modified version of [Kyty](https://github.com/InoriRus/Kyty). The project is in
active development, and behavior can change significantly between builds.

> [!IMPORTANT]
> KytyPS5 is not affiliated with Sony Interactive Entertainment or PlayStation.
> The project does not distribute games or copyrighted system software. Use
> only game files that you have obtained legally.

KytyPS5 can boot 2D games and a selection of 3D games, including titles built
with Unreal Engine 4/5, Unity, and custom engines. External low-level emulation
modules are neither required nor planned. Windows and Linux are the primary
platforms and receive the most testing.

Community test results for upstream builds are in the
[KytyPS5 Compatibility List](https://kytyps5.github.io/). Results measured with
this fork on the test platform below follow in the next section.

## Test Platform

All results in the compatibility table were measured on:

- OS: Windows 10/11 x64
- CPU: Intel Core i7-5820K
- GPU: Radeon RX 580 4GB (amdvlk driver)
- RAM: 16GB

Note: the RX 580 does not support mesh shaders, so titles that require
mesh-shader pipelines are hardware-blocked on this platform regardless of
emulator progress.

## Compatibility (This Fork, Test Platform Above)

56 titles tested: 6 InGame, 0 Unknown (reported working, awaiting
re-confirmation), 19 MainMenu, 13 Logo, 18 DoesntBoot. Dates are the test
dates (M/D/YY). Names are taken from the local dump folder names.
dates (M/D/YY). Names are taken from the local dump folder names.
dates (M/D/YY). Names are taken from the local dump folder names.

| Game | Title ID | Status | Notes |
| ---- | -------- | ------ | ----- |
| Control Ultimate Edition | PPSA01949 | InGame | Game boots to menu - Loads and gets in-game but looks dark (shader issue?) - not playable - 10/2/26 |
| Ninja Turtles Cowabunga Collection | PPSA04489 | InGame | Game Works Great! - 10/5/26 |
| PPSA08240 | PPSA08240 | InGame | Game boots but the shaders/graphic corrupt in game - 10/5/26 |
| Port Royal 4 | PPSA02815 | InGame | Boots to in-game experience (possible shader issues but playable!) - 10/2/26 |
| WWE 2K22 | PPSA02460 | InGame | Boots in game - 10/5/26 |
| Werewolf The Apocalypse - Earthblood | PPSA02113 | InGame | Boots to menu - Loads to game but game is black on HUD shows - 10/5/26 |
| Alan Wake Remastered | PPSA01925 | MainMenu | Game boots to menu - startup video plays - game crashes before reaching in-game - 10/2/26 |
| Call of the Sea | PPSA01546 | MainMenu | Game boots to menu - crashes before loading in-game - 10/2/26 |
| Chernobylite | PPSA04747 | MainMenu | Game boots to initial logo - then freezes for a while (press buttons?) - then proceeds to game menu and crashes before getting in-game - 10/2/26 |
| Five Nights at Freddys Security Breach | PPSA04677 | MainMenu | Game boots to menu - Starts video but crashes before in-game - 10/2/26 (Github states in-game) |
| Ghostwire Tokyo | PPSA01337 | MainMenu | Game boots to menu - Starts loading game then crashes before game plan - 10/2/26 |
| Grand Theft Auto San Andreas - The Definitive Edition | PPSA03524 | MainMenu | Game boots to menu - Crashes when start - 10/2/26 (Github states in-game 1 report · status: in-game · tested on 2026-09-27-421684e) |
| Grand Theft Auto Vice City - The Definitive Edition | PPSA03530 | MainMenu | Game boots to menu - Crashes when start - 10/2/26 |
| Hotwheels Unleashed | PPSA02325 | MainMenu | Game boots to menu - Crashes in menu - 10/2/26 |
| MXGP 2020 | PPSA01646 | MainMenu | Game boots to menu - graphical issue items not rendering and eventually crashes - 10/2/26 |
| Madison | PPSA07370 | MainMenu |  |
| Maneater | PPSA01862 | MainMenu | Game boots to menu - crashes in menu - 10/2/26 |
| PPSA01619 | PPSA01619 | MainMenu | Game boot to menu - crashes once making a selection to proceed in-game - 10/2/26 |
| PPSA04341 | PPSA04341 | MainMenu |  |
| PPSA04734 | PPSA04734 | MainMenu | Game boots to menu - crashes while loading to in-game - 10/5/26 |
| Resident Evil 8 - Maiden Demo | PPSA01859 | MainMenu | Boots to menu now - Muse Spark fixed no boot 10/2/26 |
| Ride 4 | PPSA01599 | MainMenu | Boots to menu - crashes when loading in menu (before attempting to start game) - 10/2/26 |
| The King of Fighters XV | PPSA02213 | MainMenu | Boots to menu - crashes before loading in-game - 10/5/26 |
| Tony Hawks Pro Skater 1 and 2 | PPSA02176 | MainMenu | Boots to menu - crashes before in-game loads - 10/5/26 |
| Train Sim World 2 Rush Hour | PPSA03944 | MainMenu | Game boots to menu - Very slow - Loading to game it crashes - 10/5/26 |
| Back 4 Blood | PPSA01695 | Logo | Game boots - game crashes before reaching menu - 10/2/26 |
| Borderlands 3 | PPSA01462 | Logo | Game boots - Gets to loading animation then crashes - 10/2/26 |
| It Takes Two | PPSA02343 | Logo | Game boots to logo - crashes before reaching menu - 10/2/26 |
| Jumanji The Video Game | PPSA03973 | Logo | Game boots to logo - crashes before reaching menu - 10/2/26 |
| LEGO Star Wars The Skywalker Saga | PPSA01865 | Logo | Game boots to logo - crashes before reaching menu - 10/2/26 |
| Observer System Redux | PPSA02118 | Logo | Game boots to a few logos - crashes before reaching menu - 10/2/26 |
| PPSA05921 | PPSA05921 | Logo | Boots loading progress bar - crashes before getting to menu - 10/5/26 |
| PPSA26786 | PPSA26786 | Logo | Boot logo - Crashes before Menu - 10/5/26 |
| Sackboy a Big Adventure | PPSA01288 | Logo | Boots to logo - starts loading but never seems to get to menu - 10/2/26 |
| Sifu | PPSA03001 | Logo | Boots logo - starts loading shows clouds but never seems to get to menu - 10/2/26 |
| Star Wars Jedi Fallen Order | PPSA02198 | Logo | Boots logo - crashes - 10/2/26 |
| Terminator Resistance Enhanced | PPSA02474 | Logo | See logo for a few seconds and crashes - 10/5/26 |
| Tropico 6 | PPSA05682 | Logo | Boots logo and loading to menu it crashes - 10/5/26 |
| A Plague Tale - Innocence | PPSA02388 | DoesntBoot | Boots with audio but no video - 10/2/26 |
| Away - The Survival Series | PPSA04555 | DoesntBoot | No Boot - 10/2/26 |
| Beyond a Steel Sky | PPSA03979 | DoesntBoot | No Boot - 10/2/26 |
| DIRT5 | PPSA01552 | DoesntBoot | No Boot - 10/2/26 |
| Fishing North Atlantic Enhanced Edition | PPSA02985 | DoesntBoot | No Boot - 10/2/26 |
| Five Nights at Freddy's Help Wanted 2 | PPSA18887 | DoesntBoot | GPU flip submission failed at boot (result=-2144796661), no video - 10/8/26 (reconstructed from log; original note lost) |
| Ghost of Tsushima Directors Cut | PPSA03208 | DoesntBoot | No Boot - 10/2/26 |
| Ghostrunner | PPSA03682 | DoesntBoot | Starts video briefly and crashed (essentially doesn't boot) - 10/2/26 |
| Grand Theft Auto III - The Definitive Edition | PPSA03527 | DoesntBoot | No Boot - 10/2/26 |
| GreedFall | PPSA02982 | DoesntBoot | No Boot - 10/2/26 |
| MARVEL Tokon Fighting Souls | PPSA15595 | DoesntBoot | Guest abort() in libC at boot, no submits - 10/8/26 (reconstructed from log; original note lost) |
| Martha is Dead | PPSA02006 | DoesntBoot | No Boot - 10/2/26 |
| Medium | PPSA03717 | DoesntBoot | No Boot - 10/5/26 |
| Planet Coaster (EU) | PPSA01735 | DoesntBoot | No Boot - 10/2/26 |
| Planet Coaster (US) | PPSA01736 | DoesntBoot | No Boot - 10/2/26 (github has 1 report · status: main-menu · tested on KytyPS5-2026-08-28-c52bf45) |
| The Ascent | PPSA02593 | DoesntBoot | Boots with audio but no video - 10/5/26 |
| The Matrix Awakens Demo | PPSA05754 | DoesntBoot | No Boot - 10/5/26 |
| The Riftbreaker | PPSA03753 | DoesntBoot | No Boot - 10/5/26 |

## System Requirements

- Windows 10 version 1803, a current Linux distribution, or macOS on Apple
  Silicon
- A 64-bit x86 processor (on macOS, Apple Silicon with Rosetta 2)
- A Vulkan 1.3-capable GPU with current drivers (on macOS, Vulkan is provided
  by the bundled MoltenVK)

## Building

### Windows

Requirements: Git, CMake 3.22.1+, Ninja, Visual Studio 2022 or Build Tools
2022 with the **Desktop development with C++** workload and **C++ Clang tools
for Windows**, Qt 6 for MSVC 2022 64-bit (Concurrent, Network, Widgets), and
`glslangValidator` on `PATH`. The MSVC compiler (`cl.exe`) is not supported;
use `clang-cl`.


### Build requirements (Windows)

- Git
- CMake 3.22.1 or newer
- Ninja
- Visual Studio 2022 or Build Tools 2022 with the **Desktop development with C++** workload and
  **C++ Clang tools for Windows** component
- Qt 6 for MSVC 2022 64-bit, including Concurrent, Network, and Widgets
- [glslang](https://github.com/KhronosGroup/glslang/releases) (`glslang` or `glslangValidator`) on `PATH`
- Python 3 on `PATH`; the bundled SPIRV-Tools runs it at configure time

The Microsoft C++ compiler (`cl.exe`) is not supported; use `clang-cl`.

Open an **x64 Native Tools Command Prompt for Visual Studio 2022** (or the equivalent Developer
PowerShell), change to the repository root, and initialize the dependencies:


```powershell
git submodule update --init --recursive
cmake -S . -B _Build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_PREFIX_PATH="C:/Qt/6.x.x/msvc2022_64"
cmake --build _Build/windows --target launcher
cmake --install _Build/windows --prefix _Build/windows/install
```

The runnable application lands in `_Build/windows/install`.

### Linux


Install the toolchain and the libraries the bundled SDL3 needs. Without the audio, Wayland and
udev development packages SDL3 quietly configures itself without those backends, and the resulting
build has no working sound and no gamepad hotplug:

```bash
sudo apt-get install --no-install-recommends \
  clang lld ninja-build cmake git glslang-tools python3 pkg-config \
  libgl1-mesa-dev libx11-dev libxcursor-dev libxext-dev libxfixes-dev \
  libxi-dev libxrandr-dev libxss-dev libxtst-dev libxkbcommon-dev \
  libasound2-dev libpulse-dev libudev-dev libdbus-1-dev libwayland-dev wayland-protocols
```

Qt 6 (Concurrent, Network, Widgets) is required for the launcher — either the distribution packages
(`qt6-base-dev`) or an official Qt installation.


```bash
git submodule update --init --recursive
cmake -S . -B _Build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_PREFIX_PATH="$Qt6_DIR"
cmake --build _Build/linux --target launcher --parallel
cmake --install _Build/linux --prefix _Build/linux/install
```

For `kyty_emulator`/`kyty_tests` without Qt, configure a separate directory
with `-DKYTY_BUILD_LAUNCHER=OFF`.

### macOS

Experimental: x86-64 builds run under Rosetta 2 on Apple Silicon (Xcode CLT,
`cmake`, `ninja`, `glslang`, universal Qt 6). Configure with
`-DCMAKE_OSX_ARCHITECTURES=x86_64`, build and install as on Linux; see
upstream docs for the MoltenVK `libMoltenVK.dylib` signing steps.


cmake -S . -B _Build/linux-no-qt -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DKYTY_BUILD_LAUNCHER=OFF

cmake --build _Build/linux-no-qt --target kyty_emulator kyty_tests --parallel
```

As on Windows, the MSVC compiler is not used; Clang is required. `cl.exe` is rejected at configure
time.

The CMake source root is the repository root.

### Building on NixOS

A development shell provides Clang, CMake, Ninja, Qt 6, the Vulkan headers, and the SDL3 backend
libraries. Enter it and configure exactly as on other Linux distributions; the shell exports
`CMAKE_PREFIX_PATH` and `QT_PLUGIN_PATH`, so the `-DCMAKE_PREFIX_PATH="$Qt6_DIR"` argument is not
needed:

```bash
nix-shell # or: nix develop
git submodule update --init --recursive

cmake -S . -B _Build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++

cmake --build _Build/linux --target launcher --parallel
cmake --install _Build/linux --prefix _Build/linux/install
```

The configure step downloads the FFmpeg prebuilts and the `xbyak`, `zydis`, `zstd`, and ZArchive
sources, so it needs network access; a fully sandboxed `nix build` would require vendoring those
inputs. A Vulkan 1.3 driver must be available at runtime (on NixOS,
`hardware.graphics.enable = true`).

### Building on macOS

macOS builds target x86-64 and run under Rosetta 2 on Apple Silicon, so the PS5's x86-64 game
code executes through the same translation layer as the emulator itself. Prebuilt archives are
attached to releases; the steps below are for building from source.

Requirements:

- An Apple Silicon Mac with Rosetta 2 installed (`softwareupdate --install-rosetta`)
- Xcode (or the Command Line Tools)
- Homebrew packages: `brew install cmake ninja glslang python`
- Qt 6 (Concurrent, Network, Widgets) with x86-64 support. The official Qt installation is
  universal and works; Homebrew's Qt is arm64-only and will not link

```bash
git submodule update --init --recursive

cmake -S . -B _Build/macos -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=x86_64 \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_PREFIX_PATH="$Qt6_DIR"

cmake --build _Build/macos --target launcher --parallel
cmake --install _Build/macos --prefix _Build/macos/install
```

The build re-signs `kyty_emulator` with the JIT entitlements it needs to execute translated
guest code; no manual signing step is required. When the launcher is built, the install
also produces `_Build/macos/install/KytyPS5.app` — double-click to launch the GUI.
A flat `kyty_emulator` is kept for CLI usage.

Vulkan comes from MoltenVK. Download `MoltenVK-macos.tar` from the
[MoltenVK releases](https://github.com/KhronosGroup/MoltenVK/releases), then copy
`MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib` next to the flat `kyty_emulator`
(and, for the bundle, into `KytyPS5.app/Contents/Frameworks/`) and ad-hoc sign it:

```bash
codesign --force --sign - _Build/macos/install/libMoltenVK.dylib
# For the bundle (if present):
codesign --force --sign - _Build/macos/install/KytyPS5.app/Contents/Frameworks/libMoltenVK.dylib
codesign --force --sign - _Build/macos/install/KytyPS5.app
```

Release archives already include a signed `libMoltenVK.dylib` (both flat and inside the bundle).

### Regression tests

Build every regression executable and run the registered tests with:


```powershell
cmake --build _Build/windows --target kyty_tests
ctest --test-dir _Build/windows --output-on-failure
```

(Use `_Build/linux` on Linux.)

## Running

Update the graphics driver before reporting rendering problems. Launch the GUI:

```powershell
.\_Build\windows\install\launcher.exe
```

```bash
./_Build/linux/install/launcher
```

On first launch, add game folders in the global settings. The launcher searches
them recursively for game directories containing `eboot.bin` and read-only
ZArchive (`.zar`) dumps, which are streamed without extraction. Or start the
emulator directly:

```powershell
.\_Build\windows\install\kyty_emulator.exe --game "D:\Games\ExampleGame"
```

```bash
./_Build/linux/install/kyty_emulator --game "/games/ExampleGame"
```

Run `kyty_emulator --help` for graphics, logging, validation, profiling, and
debugging options, including the GPU-assisted validation option added in this
fork.

## Contributing

Testing games and submitting detailed bug reports are useful contributions.
Search existing issues first, then use the **Game Emulation Status Report**
template and attach the complete log file. Include the short-hash build label
from the window title or log when reporting against this fork.

Code contributions should be focused, build on the platforms they touch, and
include tests where practical. Set up the clang-format pre-commit hook after
cloning: `python -m pip install pre-commit`, then
`python -m pre_commit install --install-hooks`.

## License

KytyPS5 is licensed under the [GNU General Public License version
2](LICENSE) (`GPL-2.0-only`). It is based on [Kyty](https://github.com/InoriRus/Kyty)
(MIT); the original notice is preserved in [`LICENSES/Kyty-MIT.txt`](LICENSES/Kyty-MIT.txt).
Third-party components keep their own licenses.

## Special Thanks

- [InoriRus/Kyty](https://github.com/InoriRus/Kyty) — the project KytyPS5 is based on.
- [shadps4-emu/shadPS4](https://github.com/shadps4-emu/shadPS4) — reference for PS4
  memory behavior, GPU resource aliasing/cache coherency, and AVPlayer.
