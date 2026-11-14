# ueshm — headless Unreal Engine driver over shared memory

`uectl` launches Unreal Engine 5.8 and drives it through one named shared-memory region. The engine side is the `UEShmBridge` plugin. Targets macOS, Windows, and Linux.

The binary speaks protocol version 1. Version 2 is specified and not implemented: a sync flag, a `step` command, an optional window, and a bulk area split between the tool and the engine. `docs/protocol.md` owns that layout.

## Architecture

The tool creates the region and the child. The plugin only opens the region. Byte offsets and orders are in `docs/protocol.md`.

```
 uectl (CLI, plain C++17)                    UE process (-nullrhi, game mode)
 ┌──────────────────────┐   shared region    ┌───────────────────────────┐
 │ launch / terminate   │  ┌──────────────┐  │ `UEShmBridge` plugin      │
 │ send cmd ───────────────► cmd ring     ├──► `FTSTicker` drains cmds   │
 │ read reply ◄────────────── rsp ring  ◄────── executes on game thread  │
 │ read bulk data ◄────────── bulk area ◄────── writes results           │
 └──────────────────────┘  └──────────────┘  └───────────────────────────┘
```

## Docs

| Doc | What it owns |
|---|---|
| `docs/protocol.md` | Header, rings, records, orders, bulk, fences |
| `docs/tool.md` | `uectl` commands, launch flags, timeouts, cleanup |
| `docs/plugin.md` | Module startup, the ticker, and the version-2 timestep and HUD |
| `docs/build.md` | CMake, the five tests, the plugin build, CI, failed runs |
| `WINDOWS.md` | Windows install and the Visual Studio debugger |

## Build and run

CMake builds `uectl` and the tests. It does not build the plugin. Unreal does not compile a missing module under `-unattended`, so build the plugin once before any engine command. Commands, generators, and CI are in `docs/build.md`.

Tool and tests, from the repo root:

```
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

On Windows the Visual Studio generator defaults to Win32, and `shm_win.cpp` is 64-bit:

```
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build --output-on-failure -C Release
```

`uectl` is `./build/uectl`, or `build\Release\uectl.exe` on Windows. The five tests are `shm_two_process`, `ring_empty`, `ring_full`, `ring_wrap`, and `ring_stress`.

Plugin, with `<UE>` as the engine root and an absolute path to the project:

```
"<UE>/Engine/Build/BatchFiles/Mac/Build.sh" UnrealEditor Mac Development -Project="<abs>/project/ueshm.uproject" -WaitMutex
"<UE>/Engine/Build/BatchFiles/Linux/Build.sh" UnrealEditor Linux Development -Project="<abs>/project/ueshm.uproject" -WaitMutex
"<UE>\Engine\Build\BatchFiles\Build.bat" UnrealEditor Win64 Development -Project="<abs>\project\ueshm.uproject" -WaitMutex
```

Run, again with an absolute project path:

```
./build/uectl demo --engine "<UE>" --project "<abs>/project/ueshm.uproject"
./build/uectl ping --engine "<UE>" --project "<abs>/project/ueshm.uproject"
./build/uectl exec "stat fps" --engine "<UE>" --project "<abs>/project/ueshm.uproject"
./build/uectl launch --engine "<UE>" --project "<abs>/project/ueshm.uproject"
```

`--map` defaults to `/Engine/Maps/Entry`. `demo` prints `spawned 1000` and exits 0. `ping` prints `pong <nonce>`. `exec` prints `ok 1`. `launch` prints the shared-memory name and waits until the process exits. The engine log contains `ueshm ready`.

`demo` exits nonzero if the ready wait exceeds 60 s, a reply exceeds 10 s, the process crashes, `spawned != 1000`, or a position check fails. The engine demo has been run on macOS. CI runs the C++ tests on Linux, macOS, and Windows. Those runners do not install the engine.

What each command sends, and what the session does on failure, is in `docs/tool.md`.

## Version 2 contract

Not in the binary. When it lands, `docs/protocol.md` is the byte layout and the two files below are the behavior.

- The header stays 128 bytes. Version becomes 2. New fields occupy the pad at offset 48, including `flags`, `frame`, and the two bulk halves.
- The tool is the only writer of the low half of bulk. The engine is the only writer of the high half. `DUMP` writes at the start of the engine half.
- `flags` bit 0 selects sync. `uectl step --dt <seconds> --count <n>` sets it, writes a speed into the tool half, and checks positions after that many stepped frames. `ping`, `exec`, `demo`, and `launch` leave it clear unless `--sync` is passed.
- `--window` drops `-nullrhi` and shows a HUD. Headless `demo` does not change: spawn count 1000, spacing 100, no mesh, no sync.

## Failed runs

- A relative `.uproject` is resolved from `Engine/Binaries/<platform>/`, not from the shell's current directory.
- Launching before the plugin is built fails under `-unattended`. The editor will not compile the missing module.
- On macOS, a relink can leave `project/Binaries/Mac/UnrealEditor.modules` naming an older `libUnrealEditor-UEShmBridge-*.dylib` than the one just linked. The editor loads the old plugin. Delete `UnrealEditor.modules` and `UnrealEditor.target` in that directory and rerun the plugin build.

Host limits that sit next to those (POSIX name length, Windows handle lifetime, Linux `/dev/shm`) are in `docs/build.md`.
