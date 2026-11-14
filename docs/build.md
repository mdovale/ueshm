# Build

CMake builds `uectl` and the tests. Unreal builds the plugin. CI runs the tests and does not install the engine. The engine run is manual.

## Tool and tests

C++17, exceptions off (`-fno-exceptions`, or MSVC `/EHs-c- /D_HAS_EXCEPTIONS=0`). CMake 3.20 or newer. Unix Makefiles on macOS and Linux. The Visual Studio generator on Windows.

From the repo root:

```
cmake -S . -B build && cmake --build build && ctest --test-dir build
```

On Windows the generator defaults to Win32, and `shm_win.cpp` is 64-bit. Pass `-A x64`, and pass the config through to the build and to `ctest`:

```
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build --output-on-failure -C Release
```

Targets:

| Target | Sources |
|---|---|
| `shm` | `common/shm_posix.cpp` or `common/shm_win.cpp` |
| `process` | `tool/process_posix.cpp` or `tool/process_win.cpp` |
| `uectl` | `tool/main.cpp`, linked to `shm` and `process` |
| `shm_test` | `tests/shm_test.cpp` |
| `ring_test` | `tests/ring_test.cpp`, linked to `Threads` |

On Linux, `shm` links `rt` when that library is present. Older glibc needs it for `shm_open`.

`uectl` is `./build/uectl`, or `build\Release\uectl.exe`.

## Tests

| ctest name | Command | What it checks |
|---|---|---|
| `shm_two_process` | `shm_test` | Parent creates a 65536-byte region, writes a pattern, execs the same binary as `child <name>`, checks the child's write, then creates the same name again after both sides have closed it |
| `ring_empty` | `ring_test empty` | Pop on an empty ring fails |
| `ring_full` | `ring_test full` | A record that would pass the capacity is refused, and the ring still drains |
| `ring_wrap` | `ring_test wrap` | A record that does not fit at the end is written at offset 0 |
| `ring_stress` | `ring_test stress` | 10000 messages, producer and consumer on two threads |

Each test has a 30 s timeout. `shm_test` with no arguments is the parent. `shm_test child <name>` is the child, spawned by the parent. `ring_test` takes one of `empty`, `full`, `wrap`, `stress`.

These tests do not start Unreal. A protocol change that the plugin must pick up is not covered here.

## Plugin

Unreal does not compile a missing module under `-unattended`. Build the plugin once before any engine command. `<UE>` is the engine root. `<abs>` is an absolute path to this repo.

```
"<UE>/Engine/Build/BatchFiles/Mac/Build.sh" UnrealEditor Mac Development -Project="<abs>/project/ueshm.uproject" -WaitMutex
"<UE>/Engine/Build/BatchFiles/Linux/Build.sh" UnrealEditor Linux Development -Project="<abs>/project/ueshm.uproject" -WaitMutex
"<UE>\Engine\Build\BatchFiles\Build.bat" UnrealEditor Win64 Development -Project="<abs>\project\ueshm.uproject" -WaitMutex
```

The module binary lands under `project/Binaries/<platform>/`. CMake does not run this step.

Windows install, the Visual Studio debugger, and the MSVC toolset are in `WINDOWS.md`.

## CI

`.github/workflows/ci.yml` runs on every push and pull request. The matrix is `ubuntu-latest`, `macos-latest`, and `windows-latest`, with `fail-fast: false`. Each job configures with `-DCMAKE_BUILD_TYPE=Release`, adds `-A x64` on Windows, builds `--config Release`, and runs `ctest --test-dir build --output-on-failure -C Release`. No engine, no plugin build.

## Runs that fail before the protocol is involved

- A relative `.uproject` is resolved from `Engine/Binaries/<platform>/`, not from the shell's current directory. `uectl` passes an absolute path. A hand-launched editor command needs one too. On macOS that directory is `Engine/Binaries/Mac/`. On Windows it is `Engine\Binaries\Win64\`.
- Launching before the plugin is built fails under `-unattended`. The editor will not compile the missing module. Build it with the `Build.sh` or `Build.bat` line above, then run `uectl`.
- On macOS, a relink can write `libUnrealEditor-UEShmBridge-NNNN.dylib` and leave `project/Binaries/Mac/UnrealEditor.modules` naming an older suffix. The editor loads the old plugin. Delete `UnrealEditor.modules` and `UnrealEditor.target` in that directory and rerun the plugin build so the manifest names the new dylib.

## Host limits

- macOS shared-memory names are limited to about 31 bytes including the slash. `sem_init` is not available. `ftruncate` is applied once per object, at create.
- Windows: keep the tool's mapping handle open until the child has closed its view. The name is `Local\ueshm-XXXXXXXX`. CMake needs `-A x64`.
- Linux: containers often mount a small `/dev/shm`. Raise it with `--shm-size` when the default 16 MiB region does not fit. Link `rt` on older glibc. CMake does that when the library is present.
- A crashed POSIX run can leave the object allocated. `Create` unlinks that name before `shm_open`. A different random name does not reclaim the stale one.
