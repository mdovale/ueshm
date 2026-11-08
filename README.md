# ueshm — headless Unreal Engine driver over shared memory

C++ tool (`uectl`) that launches Unreal Engine headless and controls it through one shared-memory region. Targets macOS, Windows, and Linux. Pinned engine: UE 5.8.

`uectl demo` exits 0 after it:

1. launches UE headless and waits for the handshake
2. sends `PING` and gets `PONG`
3. runs a console command
4. spawns 1000 actors
5. reads their 1000 positions back from the bulk area
6. shuts UE down and removes the region

Exit nonzero if the ready wait exceeds 60 s, a reply exceeds 10 s, the process crashes, `spawned != 1000`, or a position check fails. The engine demo has been run on macOS. CI runs the C++ tests on Linux, macOS, and Windows. Those runners do not install the engine.

## Build and run

CMake builds `uectl` and the tests. It does not build the plugin. Unreal does not compile a missing module under `-unattended`, so build the plugin once before any engine command.

Tool and tests, from the repo root:

```
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

On Windows the Visual Studio generator defaults to Win32, and `shm_win.cpp` is 64-bit. Pass `-A x64`, and pass the config through to the build and to `ctest`:

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

Run, again with an absolute project path. The editor resolves a relative one from its own binary directory.

```
./build/uectl demo --engine "<UE>" --project "<abs>/project/ueshm.uproject"
./build/uectl ping --engine "<UE>" --project "<abs>/project/ueshm.uproject"
./build/uectl exec "stat fps" --engine "<UE>" --project "<abs>/project/ueshm.uproject"
./build/uectl launch --engine "<UE>" --project "<abs>/project/ueshm.uproject"
```

`--map` defaults to `/Engine/Maps/Entry`. `demo` prints `spawned 1000`. `ping` prints `pong <nonce>`. `exec` prints `ok 1`. `launch` prints the shared-memory name and waits until the process exits. The engine log contains `ueshm ready`.

`--engine` is the directory that contains one of these files. The tool launches the first that exists:

- `Engine/Binaries/Mac/UnrealEditor-Cmd`
- `Engine/Binaries/Win64/UnrealEditor-Cmd.exe`
- `Engine/Binaries/Linux/UnrealEditor-Cmd`

## Architecture

```
 uectl (CLI, plain C++17)                    UE process (-nullrhi, game mode)
 ┌──────────────────────┐   shared region    ┌───────────────────────────┐
 │ launch / terminate   │  ┌──────────────┐  │ `UEShmBridge` plugin      │
 │ send cmd ───────────────► cmd ring     ├──► `FTSTicker` drains cmds   │
 │ read reply ◄────────────── rsp ring  ◄────── executes on game thread  │
 │ read bulk data ◄────────── bulk area ◄────── writes results           │
 └──────────────────────┘  └──────────────┘  └───────────────────────────┘
```

- The tool creates and owns the region and the UE child process.
- UE opens the region by name, passed as `-ShmName=<logical name>`.
- No blocking primitives. Both sides poll atomics. The tool sleeps 10 ms between polls.

## Repo layout

```
ueshm/
├─ README.md
├─ CMakeLists.txt              # C++17; shm lib, uectl, tests; exceptions off
├─ common/                     # no UE; no OS headers outside shm_*.cpp
│  ├─ protocol.h               # layout + message types
│  ├─ ring.h                   # SPSC ring (header-only)
│  ├─ shm.h                    # ShmRegion interface
│  ├─ shm_posix.cpp            # macOS + Linux
│  └─ shm_win.cpp              # Windows
├─ tool/
│  ├─ main.cpp                 # uectl: launch, demo, ping, exec
│  ├─ process.h                # Process::Launch/Wait/TryWait/Terminate
│  ├─ process_posix.cpp
│  └─ process_win.cpp
├─ project/ueshm.uproject      # content-only; enables UEShmBridge
├─ plugin/UEShmBridge/
│  ├─ UEShmBridge.uplugin      # Runtime; Win64, Mac, Linux; EngineVersion 5.8
│  └─ Source/UEShmBridge/
│     ├─ UEShmBridge.Build.cs  # include ../../../../common; compile one shm cpp
│     ├─ UEShmBridge.cpp       # module: open region, register ticker
│     ├─ Commands.h
│     └─ Commands.cpp          # command handlers
├─ tests/                      # shm_test, ring_test
└─ .github/workflows/ci.yml    # ubuntu, macos, windows; ctest only
```

`project/ueshm.uproject` is content-only and contains:

```json
"AdditionalPluginDirectories": ["../plugin"],
"Plugins": [{ "Name": "UEShmBridge", "Enabled": true }]
```

CMake compiles `common/` for `uectl` and the tests. The module cpp includes `shm_posix.cpp` or `shm_win.cpp`, so Unreal Build Tool compiles that same file. One source copy, two builds. The module include path is `common/` at the repo root. Dependencies are Core, CoreUObject, and Engine.

## Portability rules

- Platform `#ifdef`s live only in `shm_*.cpp` and `process_*.cpp`. Everything else is plain C++17. The editor path is chosen by which binary file exists.
- Shared structs: fixed-width ints, explicit alignment, little-endian, no pointers, no STL, no exceptions. Pin the layout with `static_assert(sizeof/offsetof)`.
- Atomics: `std::atomic<uint32_t>`, with `static_assert(is_always_lock_free)`. Producer releases, consumer acquires.
- Paths via `std::filesystem`. Never hard-code separators.
- Exceptions are off (`-fno-exceptions`, or MSVC `/EHs-c- /D_HAS_EXCEPTIONS=0`).

## Shared region (default 16 MiB)

The mapping is zero-filled. The creator writes the header, then release-stores `magic`. UE spins until `magic` and `version` match. Any other version leaves `state` at 0, and the tool hits the ready timeout.

Header fields, then pad to 128 bytes:

| Field | Type | Value |
|---|---|---|
| `magic` | `u32` | `0x48534555` (`UESH`) |
| `version` | `u32` | `1` |
| `total_size` | `u32` | mapped bytes |
| `state` | atomic `u32` | `0` created, `1` ue_ready, `2` closing |
| `tool_heartbeat` | atomic `u32` | tool bumps every poll |
| `ue_heartbeat` | atomic `u32` | plugin bumps every tick |
| `cmd_off`, `cmd_bytes` | `u32` | command ring |
| `rsp_off`, `rsp_bytes` | `u32` | reply ring |
| `bulk_off`, `bulk_bytes` | `u32` | bulk area |

`cmd_off` and `rsp_off` point at that ring's `head`. `cmd_bytes` and `rsp_bytes` are the data capacity, 65536. `bulk_off` follows the reply ring. With the default 16 MiB region those offsets are 128, 65920, and 131712.

Each ring is `head` padded to 128 bytes, `tail` padded to 128 bytes, then 64 KiB of data. `head` is the producer byte offset, `tail` the consumer byte offset. Both are monotonic `uint32_t`; subtract them unsigned. The data address is `index & 65535`. Empty when `head == tail`. A record is never split: if the bytes left at the end are shorter than the record, that tail is skipped and the record is written at offset 0. Full when the next record, including a skipped tail, would push `head - tail` past 65536.

UE writes each dump at the start of the bulk area, then pushes `DONE`. The tool reads bulk only after the matching reply.

## Wire protocol

Each entry is a 16-byte prefix, the payload, then pad to 8 bytes:

`u32 size | u16 type | u16 pad | u64 id | payload`

`pad` is zero so `id` is 8-byte aligned. `size` is the prefix plus the payload, excluding the trailing pad. `id` is a tool counter starting at 1. Replies copy `id`. Max payload is 1024 bytes. Unknown type: reply `ERR` (`104`) with `u32 type` and the same `id`.

| Type | Value | Payload | Reply |
|---|---|---|---|
| `PING` | `1` | `u64 nonce` | `PONG` (`101`) + nonce |
| `CONSOLE` | `2` | raw UTF-8, no NUL | `ACK` (`102`) + `u32 ok` |
| `SPAWN` | `3` | `u32 count`, `f32 spacing` | `ACK` + `u32 spawned` |
| `DUMP` | `4` | empty | `DONE` (`103`) + `u32 bytes` |
| `QUIT` | `5` | empty | `ACK` |

`CONSOLE` calls `GEngine->Exec` on the game world. `ok` is 1 when that returns true. `demo` sends `stat fps`.

`SPAWN` spawns into that world, in order. Actor `i` is a movable `AStaticMeshActor` with no mesh at world position `(i * spacing, 0, 0)`. The actor is spawned at the origin, the mesh component is set movable, then the location is applied. A static component would ignore the location. Collision handling is `AlwaysSpawn`. Count is capped to what the bulk area can hold, `(bulk_bytes - 4) / 12`. A later `SPAWN` replaces the previous actors. `demo` uses `count = 1000` and `spacing = 100` (cm).

`DUMP` writes `u32 count`, then `count × 3 × f32` positions in spawn order. `DONE.bytes` equals `4 + count * 12`. The tool checks `|x - i * spacing| <= 0.01` and `y == z == 0`. A dump that does not fit in the bulk area gets `ERR`.

`QUIT` pushes `ACK`, then calls `FPlatformMisc::RequestExit(false)`. The handler returns after that, and the ticker stops draining.

## Components

**`common/shm`**: `ShmRegion::Create(name, size)`, `Open(name)`, `Data()`, `Size()`. The logical name is `ueshm-<8 hex>`. POSIX object is `/ueshm-XXXXXXXX`. Windows mapping is `Local\ueshm-XXXXXXXX`. Destructor unmaps. The creator also removes the object.
- POSIX: `shm_unlink` the stale name, then `shm_open` (`O_CREAT|O_EXCL`), `ftruncate`, `mmap`. Unlink again after the child is gone.
- Windows: `CreateFileMappingA` + `MapViewOfFile`. `SetLastError(0)` before create so `ERROR_ALREADY_EXISTS` is meaningful. Open reads the size with `NtQuerySection` (`SectionBasicInformation`) and falls back to `VirtualQuery`. Close the handle after the child is gone. `shm_win.cpp` is Win64 only.

**`uectl`**: creates the region, calls `InitRegion`, then launches:

```
UnrealEditor-Cmd <proj>.uproject <map> -game -nullrhi -unattended -nosplash -nopause -stdout -ShmName=<logical name>
```

The project path passed to the editor is absolute. `launch` waits for `ue_ready`, prints the name, and waits until the process exits. `ping`, `exec`, and `demo` wait for `ue_ready`, run their requests, and send `QUIT`. `demo` checks the ping nonce, requires `stat fps` to return `ok` 1, then runs the spawn and the position check. After `QUIT` the tool waits up to 60 s for exit code 0. It then sets `state = closing` and removes the region. On timeout or crash it terminates the child and does the same cleanup. If the child is still alive when `uectl` exits, the session kills it first.

**`UEShmBridge`**: `StartupModule` reads `-ShmName`. With no name, the module returns and registers no ticker. Otherwise it opens the region and registers an `FTSTicker`. It sets `state = ue_ready` on the first tick where a `UWorld` of type Game or PIE exists, and logs `ueshm ready` once. Each tick bumps `ue_heartbeat` and drains at most 64 commands on the game thread.

**Tests**: `shm_test` is the two-process test. The parent creates a 65536-byte region, writes a pattern, execs the same binary as `child <name>`, checks the child's write, and creates the same name again after both sides have closed it. `ring_test` takes `empty`, `full`, `wrap`, or `stress`. The stress run is 10000 messages on two threads. CI is cmake and `ctest` on all three runners. The engine run stays manual.

## Gotchas

- **Project path:** pass an absolute `.uproject`. A relative path is resolved from `Engine/Binaries/<platform>/`, not from the shell's current directory.
- **macOS plugin binary:** a relink can write `libUnrealEditor-UEShmBridge-NNNN.dylib` and leave `project/Binaries/Mac/UnrealEditor.modules` pointing at an older suffix. The editor then loads the old plugin. Delete `UnrealEditor.modules` and `UnrealEditor.target` in that directory and rerun the plugin build so the manifest names the new dylib.
- **macOS shm:** names are limited to about 31 chars including the slash. `/ueshm-` plus 8 hex fits. `sem_init` is unsupported. `ftruncate` works once per object.
- **Windows:** the mapping disappears with the last handle, so keep the tool's handle open until UE has closed. CMake needs `-A x64`.
- **Linux:** containers have a small `/dev/shm` by default; use `--shm-size` in Docker. Older glibc needs `-lrt`. CMake links `rt` when that library is present.
- **Rendering:** `-nullrhi` disables the renderer. The demo spawns actors with no mesh. A rendered run needs `-RenderOffscreen` and a GPU, per platform. On macOS, `-nullrhi` plus this spawn needed no extra flags.
- **Commandlets:** they exit when finished, and some plugins do not start in them. The demo launches game mode.
- **Stale regions:** a crashed run can leave a POSIX object behind. Unlink-before-create clears it.
