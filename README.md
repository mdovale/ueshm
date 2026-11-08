# ueshm — headless Unreal Engine driver over shared memory

C++ tool (`uectl`) that launches Unreal Engine headless and controls it through one shared-memory region. Targets macOS, Windows, and Linux. Pinned engine: UE 5.8.

## Goal

`uectl demo` must, on all three OSes, exit 0 after it:

1. launches UE headless and waits for the handshake
2. sends `PING` and gets `PONG`
3. runs a console command
4. spawns 1000 actors
5. reads their 1000 positions back from the bulk area
6. shuts UE down and removes the region

Exit nonzero if the ready wait exceeds 60 s, a reply exceeds 10 s, the process crashes, `spawned != 1000`, or a position check fails.

## Build and run

```
cmake -S . -B build && cmake --build build && ctest --test-dir build
uectl demo --engine <UE root> --project project/ueshm.uproject --map /Engine/Maps/Entry
```

`ping`, `exec "<cmd>"`, and `launch` take the same `--engine`, `--project`, and `--map` flags. The tool generates the shm name. Default map is `/Engine/Maps/Entry`.

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
│  ├─ process.h                # Process::Launch/Wait/Terminate
│  ├─ process_posix.cpp
│  └─ process_win.cpp
├─ project/ueshm.uproject      # content-only; enables UEShmBridge
├─ plugin/UEShmBridge/
│  ├─ UEShmBridge.uplugin      # Runtime; Win64, Mac, Linux; EngineVersion 5.8
│  └─ Source/UEShmBridge/
│     ├─ UEShmBridge.Build.cs  # include ../../../../common; compile one shm cpp
│     ├─ UEShmBridge.cpp       # module: open region, register ticker
│     └─ Commands.cpp          # command handlers
├─ tests/                      # ctest: shm two-process, ring wrap/full/empty
└─ .github/workflows/ci.yml    # matrix: ubuntu, macos, windows
```

`project/ueshm.uproject` is content-only and contains:

```json
"AdditionalPluginDirectories": ["../plugin"],
"Plugins": [{ "Name": "UEShmBridge", "Enabled": true }]
```

CMake compiles `common/` for `uectl` and the tests. The module cpp includes `shm_posix.cpp` or `shm_win.cpp`, so Unreal Build Tool compiles that same file. One source copy, two builds.

## Portability rules

- Platform `#ifdef`s live only in `shm_*.cpp` and `process_*.cpp`. Everything else is plain C++17.
- Shared structs: fixed-width ints, explicit alignment, little-endian, no pointers, no STL, no exceptions. Pin the layout with `static_assert(sizeof/offsetof)`.
- Atomics: `std::atomic<uint32_t>`, with `static_assert(is_always_lock_free)`. Producer releases, consumer acquires.
- Paths via `std::filesystem`. Never hard-code separators.
- `--engine <root>` selects the binary:
  - macOS: `Engine/Binaries/Mac/UnrealEditor-Cmd`
  - Windows: `Engine\Binaries\Win64\UnrealEditor-Cmd.exe`
  - Linux: `Engine/Binaries/Linux/UnrealEditor-Cmd`

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

`cmd_off` and `rsp_off` point at that ring's `head`. `cmd_bytes` and `rsp_bytes` are the data capacity, 65536. `bulk_off` follows the reply ring.

Each ring is `head` padded to 128 bytes, `tail` padded to 128 bytes, then 64 KiB of data. `head` is the producer byte offset, `tail` the consumer byte offset. Both are monotonic `uint32_t`; subtract them unsigned. The data address is `index & 65535`. Empty when `head == tail`. A record is never split: if the bytes left at the end are shorter than the record, that tail is skipped and the record is written at offset 0. Full when the next record, including a skipped tail, would push `head - tail` past 65536.

UE writes each dump at the start of the bulk area. The tool reads bulk only after the matching reply.

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

`SPAWN` spawns into that world, in order. Actor `i` is a movable `AStaticMeshActor` with no mesh at world position `(i * spacing, 0, 0)`. `demo` uses `count = 1000` and `spacing = 100` (cm).

`DUMP` writes `u32 count`, then `count × 3 × f32` positions in spawn order. `DONE.bytes` equals `4 + count * 12`. The tool checks `|x - i * spacing| <= 0.01` and `y == z == 0`.

`QUIT` pushes `ACK`, then calls `FPlatformMisc::RequestExit(false)`.

## Components

**`common/shm`**: `ShmRegion::Create(name, size)`, `Open(name)`, `Data()`, `Size()`. The logical name is `ueshm-<8 hex>`. POSIX object is `/ueshm-XXXXXXXX`. Windows mapping is `Local\ueshm-XXXXXXXX`. Destructor unmaps. The creator also removes the object.
- POSIX: `shm_unlink` the stale name, then `shm_open` (`O_CREAT|O_EXCL`), `ftruncate`, `mmap`. Unlink again after the child is gone.
- Windows: `CreateFileMappingA` + `MapViewOfFile`. Close the handle after the child is gone.

**`uectl`**: creates the region, then launches:

```
UnrealEditor-Cmd <proj>.uproject <map> -game -nullrhi -unattended -nosplash -nopause -stdout -ShmName=<logical name>
```

`launch` waits for `ue_ready`, prints the name, and waits until the process exits. `ping`, `exec`, and `demo` wait for `ue_ready`, run their requests, and send `QUIT`. After the process exits, the tool sets `state = closing` and removes the region. On timeout or crash it terminates the child and does the same cleanup.

**`UEShmBridge`**: `StartupModule` reads `-ShmName`, opens the region, and registers an `FTSTicker`. It sets `state = ue_ready` on the first tick where the game `UWorld` exists. Each tick bumps `ue_heartbeat`, drains at most 64 commands, runs them on the game thread, and pushes replies.

**Tests**: one binary. The parent creates the region and execs the same binary with the name. Ring wrap, full, and empty run in-process on two threads. CI is cmake and `ctest` on all three runners. The UE run is manual.

## Milestones

Each milestone builds and can be tested alone.

0. **Spike (UE only).** The content-only project and an empty plugin. A ticker logs a line under the launch command, on macOS first.
1. **shm lib.** Two-process test passes on all three CI runners.
2. **Rings + protocol.** Wrap, full, empty, and a two-thread stress run.
3. **Handshake + PING + CONSOLE** against UE 5.8.
4. **SPAWN + DUMP.** The 1000-actor demo, with the position check above.
5. **CI.** Matrix build and `ctest` on all three OSes. The engine run stays manual.

## Gotchas

- **macOS:** shm names are limited to about 31 chars including the slash. `/ueshm-` plus 8 hex fits. `sem_init` is unsupported. `ftruncate` works once per object.
- **Windows:** the mapping disappears with the last handle, so keep the tool's handle open until UE has closed.
- **Linux:** containers have a small `/dev/shm` by default; use `--shm-size` in Docker. Older glibc needs `-lrt`.
- **Rendering:** `-nullrhi` disables the renderer. The demo spawns actors with no mesh. A rendered run needs `-RenderOffscreen` and a GPU, per platform.
- **Commandlets:** they exit when finished, and some plugins do not start in them. The demo launches game mode.
- **Stale regions:** a crashed run can leave a POSIX object behind. Unlink-before-create clears it.

## Verify early (not confirmed)

- The UE 5.8 binary paths above match the install.
- `UnrealEditor-Cmd ... -game -nullrhi` keeps ticking and spawns actors on 5.8. Milestone 0 covers the ticking half.
- `-nullrhi` plus actor spawn needs no extra flags on all three platforms.
