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

54 titles tested: 4 InGame, 2 Unknown (reported working, awaiting
re-confirmation), 19 MainMenu, 13 Logo, 16 DoesntBoot. Dates are the test
dates (M/D/YY). Names are taken from the local dump folder names.

| Game | Title ID | Status | Notes |
| ---- | -------- | ------ | ----- |
| Alone in the Dark | PPSA08240 | InGame | Boots, but shaders/graphics corrupt in-game - 10/5/26 |
| Control Ultimate Edition | PPSA01949 | InGame | Menu loads; in-game but dark (shader issue?), not playable - 10/2/26 |
| Werewolf The Apocalypse - Earthblood | PPSA02113 | InGame | Menu loads; in-game video black, HUD visible - 10/5/26 |
| WWE 2K22 | PPSA02460 | InGame | Boots in-game - 10/5/26 |
| Ninja Turtles Cowabunga Collection | PPSA04489 | Unknown | Works great - 10/5/26 |
| Port Royal 4 | PPSA02815 | Unknown | In-game experience, possible shader issues but playable - 10/2/26 |
| Alan Wake Remastered | PPSA01925 | MainMenu | Startup video plays, crashes before in-game - 10/2/26 |
| Avatar The Last Airbender - Quest for Balance | PPSA04341 | MainMenu | No notes recorded |
| Call of the Sea | PPSA01546 | MainMenu | Crashes before loading in-game - 10/2/26 |
| Chernobylite | PPSA04747 | MainMenu | Freezes after logo, then menu; crashes before in-game - 10/2/26 |
| Five Nights at Freddys Security Breach | PPSA04677 | MainMenu | Starts video, crashes before in-game; upstream reports in-game - 10/2/26 |
| Ghostwire Tokyo | PPSA01337 | MainMenu | Starts loading, crashes before gameplay - 10/2/26 |
| Grand Theft Auto San Andreas - The Definitive Edition | PPSA03524 | MainMenu | Crashes on start; upstream reports in-game (2026-09-27-421684e) - 10/2/26 |
| Grand Theft Auto Vice City - The Definitive Edition | PPSA03530 | MainMenu | Crashes on start - 10/2/26 |
| Hotwheels Unleashed | PPSA02325 | MainMenu | Crashes in menu - 10/2/26 |
| Madison | PPSA07370 | MainMenu | No notes recorded |
| Maneater | PPSA01862 | MainMenu | Crashes in menu - 10/2/26 |
| Mortal Kombat 11 Ultimate | PPSA01619 | MainMenu | Crashes after menu selection - 10/2/26 |
| MX vs ATV Legends | PPSA04734 | MainMenu | Crashes while loading in-game - 10/5/26 |
| MXGP 2020 | PPSA01646 | MainMenu | Items not rendering, eventually crashes - 10/2/26 |
| Resident Evil 8 - Maiden Demo | PPSA01859 | MainMenu | Reaches menu (prior no-boot fixed) - 10/2/26 |
| Ride 4 | PPSA01599 | MainMenu | Crashes loading inside menu - 10/2/26 |
| The King of Fighters XV | PPSA02213 | MainMenu | Crashes before loading in-game - 10/5/26 |
| Tony Hawks Pro Skater 1 and 2 | PPSA02176 | MainMenu | Crashes before in-game loads - 10/5/26 |
| Train Sim World 2 Rush Hour | PPSA03944 | MainMenu | Very slow; crashes loading in-game - 10/5/26 |
| Back 4 Blood | PPSA01695 | Logo | Crashes before menu - 10/2/26 |
| Borderlands 3 | PPSA01462 | Logo | Loading animation, then crashes - 10/2/26 |
| Indiana Jones and The Great Circle | PPSA26786 | Logo | Crashes before menu - 10/5/26 |
| It Takes Two | PPSA02343 | Logo | Crashes before menu - 10/2/26 |
| Jumanji The Video Game | PPSA03973 | Logo | Crashes before menu - 10/2/26 |
| LEGO Star Wars The Skywalker Saga | PPSA01865 | Logo | Crashes before menu - 10/2/26 |
| Observer System Redux | PPSA02118 | Logo | Past a few logos, crashes before menu - 10/2/26 |
| Sackboy a Big Adventure | PPSA01288 | Logo | Starts loading, never reaches menu - 10/2/26 |
| Sifu | PPSA03001 | Logo | Loading clouds, never reaches menu - 10/2/26 |
| Star Wars Jedi Fallen Order | PPSA02198 | Logo | Crashes after logo - 10/2/26 |
| Terminator Resistance Enhanced | PPSA02474 | Logo | Logo for a few seconds, then crashes - 10/5/26 |
| The Devil In Me | PPSA05921 | Logo | Loading progress bar, crashes before menu - 10/5/26 |
| Tropico 6 | PPSA05682 | Logo | Crashes loading toward menu - 10/5/26 |
| A Plague Tale - Innocence | PPSA02388 | DoesntBoot | Audio but no video - 10/2/26 |
| Away - The Survival Series | PPSA04555 | DoesntBoot | No boot - 10/2/26 |
| Beyond a Steel Sky | PPSA03979 | DoesntBoot | No boot - 10/2/26 |
| DIRT5 | PPSA01552 | DoesntBoot | No boot - 10/2/26 |
| Fishing North Atlantic Enhanced Edition | PPSA02985 | DoesntBoot | No boot - 10/2/26 |
| Ghost of Tsushima Directors Cut | PPSA03208 | DoesntBoot | No boot - 10/2/26 |
| Ghostrunner | PPSA03682 | DoesntBoot | Brief video, then crashes - 10/2/26 |
| Grand Theft Auto III - The Definitive Edition | PPSA03527 | DoesntBoot | No boot - 10/2/26 |
| GreedFall | PPSA02982 | DoesntBoot | No boot - 10/2/26 |
| Martha is Dead | PPSA02006 | DoesntBoot | No boot - 10/2/26 |
| Medium | PPSA03717 | DoesntBoot | No boot - 10/5/26 |
| Planet Coaster (EU) | PPSA01735 | DoesntBoot | No boot - 10/2/26 |
| Planet Coaster (US) | PPSA01736 | DoesntBoot | No boot here; upstream reports main-menu (KytyPS5-2026-08-28-c52bf45) - 10/2/26 |
| The Ascent | PPSA02593 | DoesntBoot | Audio but no video - 10/5/26 |
| The Matrix Awakens Demo | PPSA05754 | DoesntBoot | No boot - 10/5/26 |
| The Riftbreaker | PPSA03753 | DoesntBoot | No boot - 10/5/26 |

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

From an **x64 Native Tools Command Prompt for VS 2022** in the repo root
(replace the Qt path with the installed version):

```powershell
git submodule update --init --recursive
cmake -S . -B _Build/windows -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_PREFIX_PATH="C:/Qt/6.x.x/msvc2022_64"
cmake --build _Build/windows --target launcher
cmake --install _Build/windows --prefix _Build/windows/install
```

The runnable application lands in `_Build/windows/install`.

### Linux

Install Clang, CMake, Ninja, glslang, Qt 6 (Concurrent, Network, Widgets), and
the SDL3 backend dev packages (audio, Wayland, udev — without them the build
silently lacks sound and gamepad hotplug). Then:

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

### Regression Tests

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
