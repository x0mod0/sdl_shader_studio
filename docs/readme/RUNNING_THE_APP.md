# Running the app

This guide takes you from a fresh clone to a running SDL Shader Studio. It starts
with the one-command route, then shows the same build done by hand on each
platform, and explains the parts that usually go wrong, so you can fix them
yourself when they do.

## Contents

- [Quick start](#quick-start)
- [Prerequisites](#prerequisites)
- [Building by hand](#building-by-hand)
  - [macOS](#macos)
  - [Linux](#linux)
  - [Windows](#windows)
- [Configure options](#configure-options)
- [How SDL_shadercross is reached](#how-sdl_shadercross-is-reached)
- [Troubleshooting](#troubleshooting)

---

## Quick start

`scripts/` has one script per platform. Each one checks your environment, builds
only when something has changed, and then launches the app:

```sh
./scripts/run-macos.sh                  # macOS
./scripts/run-linux.sh                  # Linux
.\scripts\run-windows.ps1               # Windows (PowerShell)
```

Anything you pass that is not an option goes to the app, so this starts with the
example project open:

```sh
./scripts/run-macos.sh examples/hello-shader
```

| What you want | macOS / Linux | Windows |
|---|---|---|
| Build even if nothing changed | `--force` | `-Force` |
| Delete `build/` and configure from scratch | `--clean` | `-Clean` |
| Build Debug instead of Release | `--debug` | `-DebugBuild` |
| Show the script's help | `--help` | `Get-Help .\scripts\run-windows.ps1` |

If a tool is missing, the script names it and prints the command that installs
it. You get that message immediately, rather than a CMake failure a few minutes
into the build.

The rest of this page does the same thing by hand. Read it when you want to
understand the build, change how it is configured, or diagnose a problem.

---

## Prerequisites

| | Requirement |
|---|---|
| Compiler | C++20: GCC 12+, Clang 15+, MSVC 19.36+ (VS 2022 17.6) |
| Build system | CMake 3.21 or newer |
| Git | Needed for dependency fetching |
| Graphics | Any SDL GPU backend: Vulkan, Metal or D3D12 |

**Dependencies are handled for you.** SDL3, SDL_shadercross, Dear ImGui, toml++,
and optionally glslang, LZ4 and zstd are used from your system if they are
installed, and downloaded automatically if not. That is why the **first configure
takes a few minutes**. Later configures are fast.

**Ninja is optional.** The commands below use CMake's default generator, which
needs only a compiler and CMake. If you have [Ninja](https://ninja-build.org)
installed, adding `-G Ninja` to the configure step gives noticeably faster
incremental builds:

```sh
brew install ninja                 # macOS
sudo apt install ninja-build       # Debian / Ubuntu
winget install Ninja-build.Ninja   # Windows

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```

Nothing else in the project depends on Ninja.

> **Watch out:** CMake stores the generator in `build/CMakeCache.txt` on the
> first configure. If you later run configure with a different generator, CMake
> refuses instead of switching. Delete the build directory (`rm -rf build`) and
> configure again.

---

## Building by hand

Every platform follows the same three steps. Knowing what each step does makes
the error messages much easier to read:

1. **Configure** - `cmake -B build ...` finds your compiler and dependencies and
   writes build files into `build/`. Most environment problems appear here.
2. **Build** - `cmake --build build` compiles the app, the `ssstudio` command
   line tool and the tests.
3. **Test** - `ctest --test-dir build` runs the test suite. It is a quick way to
   confirm the build works before you launch anything.

Clone or download the project first, and run the commands from its root folder
(the one that contains `CMakeLists.txt`).

### macOS

```sh
xcode-select --install                 # command line tools, if you have not already
brew install cmake

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

"./build/bin/SS Studio.app/Contents/MacOS/SS Studio"                       # start the app
"./build/bin/SS Studio.app/Contents/MacOS/SS Studio" examples/hello-shader # with a project
```

- **Metal is selected automatically.**
- **`.metallib` output needs full Xcode**, not only the command line tools,
  because `xcrun metal` comes with Xcode.

### Linux

```sh
# Debian / Ubuntu
sudo apt install build-essential cmake git \
     libx11-dev libxext-dev libwayland-dev libxkbcommon-dev libegl1-mesa-dev \
     vulkan-tools libvulkan-dev

# Fedora
sudo dnf install gcc-c++ cmake git \
     libX11-devel wayland-devel libxkbcommon-devel mesa-libEGL-devel vulkan-devel

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure

"./build/bin/SS Studio"
```

The X11, Wayland and EGL packages are what SDL3 needs to open a window. The
Vulkan packages give the preview a GPU backend. Check yours with `vulkaninfo`.

### Windows

From a **Developer PowerShell for VS 2022**:

```powershell
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure

& ".\build\bin\Release\SS Studio.exe"
```

Visual Studio is a *multi-config* generator: the build type is chosen at build
time with `--config`, not at configure time. That is why the binary ends up
under `bin\Release\`, and why `ctest` needs `-C Release`.

With MSYS2 or MinGW instead:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
"./build/bin/SS Studio.exe"
```

**DXIL output needs two extra DLLs**, `dxcompiler.dll` and `dxil.dll`.
SDL_shadercross ships them. Where they need to be depends on how SDL_shadercross
is reached (see [How SDL_shadercross is reached](#how-sdl_shadercross-is-reached)):

- **Release download (run as a tool):** they are already beside
  `shadercross.exe`. Nothing to do.
- **Linked into the app:** they are loaded into the app's own process, so they
  must sit next to `SS Studio.exe`. The build does **not** copy them there for
  you.

---

## Configure options

Pass these to the configure step, for example
`cmake -B build -DSSSTUDIO_WITH_ZSTD=ON`.

| Option | Default | Effect |
|---|---|---|
| `-DSSSTUDIO_BUILD_GUI=OFF` | ON | CLI and tests only; no SDL or ImGui needed |
| `-DSSSTUDIO_WITH_GLSLANG=OFF` | ON | Drop the GLSL front end |
| `-DSSSTUDIO_WITH_SHADERCROSS=OFF` | ON | Drop HLSL compilation and translation |
| `-DSSSTUDIO_SHADERCROSS_MODE=library\|cli` | auto | Link SDL_shadercross, or run its command line tool (see below) |
| `-DSSSTUDIO_WITH_LZ4=OFF` | ON | Drop LZ4 pack compression |
| `-DSSSTUDIO_WITH_ZSTD=ON` | OFF | Add zstd pack compression |
| `-DSSSTUDIO_BUILD_TESTS=OFF` | ON | Skip the test target |

Some common combinations:

```sh
# A CI machine that only builds packs: no windowing stack at all
cmake -B build -DSSSTUDIO_BUILD_GUI=OFF -DSSSTUDIO_BUILD_TESTS=OFF

# zstd compression, which usually makes smaller packs than LZ4
cmake -B build -DSSSTUDIO_WITH_ZSTD=ON
```

---

## How SDL_shadercross is reached

SDL_shadercross is the library that turns HLSL into SPIR-V, DXIL, DXBC and MSL.
The app can use it in two ways, and choosing the wrong one fails in a way that is
hard to read, so **the build chooses for you**. This section explains that choice
so you know what to expect.

### The problem: two copies of SDL3

A **release download** of SDL_shadercross includes the SDL3 it was built against,
and loads it through its own rpath. If the app linked that copy, the process would
contain **two SDL3s**. They do not share the error buffer that the shader compiler
reports through, so a failed HLSL compile would arrive with no message, no file
and no line. It would only say that the shader is wrong.

### The two modes

| Mode | Chosen when | How it works |
|---|---|---|
| **`cli`** | The build finds an SDL3 next to the SDL_shadercross it found (a release download) | Runs the `shadercross` command line tool, one process per compile. The app keeps one SDL3, and you get DXC's real output with line numbers. |
| **`library`** | SDL_shadercross was built as part of this project | Links it directly. It shares this project's SDL3, so the problem above does not occur. |

To override the automatic choice, pass `-DSSSTUDIO_SHADERCROSS_MODE=library` or
`=cli`.

### The toolchain ships with the app

In `cli` mode, the build copies the `shadercross` tool and the libraries it loads
into the build output:

- **macOS:** `Contents/Resources/tools/shadercross` inside the app bundle.
- **Everywhere else:** a `tools/shadercross` directory next to the executable.

So if you give someone a copy of the app, they can compile HLSL without
installing anything.

The `bin/` and `lib/` layout inside that directory is kept as it is, not
flattened. The tool finds its libraries through an rpath of
`@executable_path/../lib`, so they must stay one directory over. Keeping them
there also stops the toolchain's SDL3 from landing next to the app's own SDL3,
which is the exact problem the separate process exists to avoid.

### Where the app looks for the tool

At runtime, the app searches these places in order:

1. **Settings > Tools > shadercross dir**
2. The copy bundled with the app
3. Where the build found the package
4. `PATH`

The setting comes first so you can swap in a newer toolchain, or one built for a
different target, without reconfiguring. The bundled copy comes before the
build's own path because that path is absolute, so it is meaningless on any
machine other than the one that did the build.

The `ssstudio` command line tool reads the same setting, so a build from the
terminal uses the same toolchain as the app.

---

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `CMAKE_CXX_COMPILER not set, after EnableLanguage` (macOS) | CMake cannot find a compiler. Run `xcode-select --install`. If Xcode is installed but not selected, run `sudo xcode-select --switch /Applications/Xcode.app`. Check with `clang --version`, then configure again. |
| CMake refuses to configure after you added or removed `-G Ninja` | The generator is fixed on first configure. `rm -rf build` and configure again. |
| `vulkaninfo` fails (Linux) | Install your GPU vendor's Vulkan driver. The app still starts and compiles shaders without it; the preview reports that no device is available instead of crashing. |
| DXIL builds fail on Windows | `dxcompiler.dll` and `dxil.dll` are not where they need to be. See [Windows](#windows). |
| An HLSL compile fails with no message, file or line | Two SDL3s are loaded. Reconfigure with `-DSSSTUDIO_SHADERCROSS_MODE=cli`. See [How SDL_shadercross is reached](#how-sdl_shadercross-is-reached). |
| `.metallib` is not produced (macOS) | Install full Xcode; the command line tools do not include `xcrun metal`. |
| Something else misbehaves during the build | Configure with `-DSSSTUDIO_BUILD_GUI=OFF`. This build has no windowing dependencies, so if it works the problem is in the GUI stack, and if it fails the problem is in the shader pipeline. |

---

**Next:** [What it does](WHAT_IT_DOES.md) ·
[The command line tool](COMMAND_LINE.md)
