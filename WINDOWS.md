# Install and run on Windows

Visual Studio builds `uectl` and the five tests. The Unreal plugin is a second build, done by Unreal's own build script with the same Visual Studio C++ tools.

## 1. Install the tools

Install Visual Studio 2022 17.8 or newer, or Visual Studio 2026. In the installer, select **Desktop development with C++**. This engine wants the MSVC toolset **14.44** (Visual Studio 2022 17.14) or **14.50** (Visual Studio 2026), and a Windows SDK. The preferred SDK is 10.0.22621. CMake comes with that workload.

For the engine demo, install Unreal Engine 5.8 as well. The folder you will pass as `--engine` is the one that contains `Engine\Binaries\Win64\UnrealEditor-Cmd.exe`, usually `C:\Program Files\Epic Games\UE_5.8`.

## 2. Get the source

```
git clone git@github.com:mdovale/ueshm.git
cd ueshm
```

## 3. Configure and build `uectl` in Visual Studio

Open **x64 Native Tools Command Prompt for VS 2022** (or the VS 2026 equivalent) and run this from the repo root:

```
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build --output-on-failure -C Release
```

`-A x64` matters. The Visual Studio generator defaults to Win32, and the Windows shared-memory code is 64-bit. All five tests should pass. `uectl.exe` is `build\Release\uectl.exe`.

Then open `build\ueshm.sln` in Visual Studio. At the top, set the configuration to **Release** and the platform to **x64**. Right-click the `uectl` project and choose **Set as Startup Project**.

To run the demo under the debugger, open the `uectl` project properties, go to **Debugging**, and set **Command Arguments** to:

```
demo --engine "C:\Program Files\Epic Games\UE_5.8" --project "C:\full\path\to\ueshm\project\ueshm.uproject"
```

Use your real engine folder and an absolute path to the `.uproject`. **Working Directory** can stay as the repo root. Do not press F5 yet. The plugin is not built.

## 4. Build the plugin

This does not happen when you build the solution. From the same prompt:

```
"C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat" UnrealEditor Win64 Development -Project="C:\full\path\to\ueshm\project\ueshm.uproject" -WaitMutex
```

The first build takes a few minutes. The output is a DLL under `project\Binaries\Win64\`.

## 5. Run the demo

From `build\Release`, or with F5 using the arguments from step 3:

```
uectl.exe demo --engine "C:\Program Files\Epic Games\UE_5.8" --project "C:\full\path\to\ueshm\project\ueshm.uproject"
```

A good run prints `spawned 1000`, exits 0, and the engine log contains `ueshm ready`. The engine log is in that same terminal. The run takes a bit over ten seconds once the plugin is built.

`ping`, `exec "stat fps"`, and `launch` take the same `--engine` and `--project` arguments. `launch` starts the engine and waits until the process exits.
