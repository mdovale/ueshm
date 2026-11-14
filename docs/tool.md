# uectl

`uectl` is a C++17 process. It creates the shared region, publishes the header, starts Unreal, and polls. The layout it publishes is `docs/protocol.md`. This file owns the commands, the launch line, the timeouts, and cleanup.

The binary is `./build/uectl`, or `build\Release\uectl.exe` on Windows.

## Arguments

```
uectl launch|ping|demo --engine <root> --project <uproject> [--map <map>]
uectl exec <command> --engine <root> --project <uproject> [--map <map>]
```

`--engine` and `--project` are required. `--map` defaults to `/Engine/Maps/Entry`. `exec` joins its remaining non-option arguments with spaces and sends that string. A missing command, an unknown subcommand, or a stray argument on `launch`, `ping`, or `demo` prints usage and exits 1.

`--engine` is the directory that contains one of these files. The tool launches the first that exists:

- `Engine/Binaries/Mac/UnrealEditor-Cmd`
- `Engine/Binaries/Win64/UnrealEditor-Cmd.exe`
- `Engine/Binaries/Linux/UnrealEditor-Cmd`

The project path passed to the editor is `std::filesystem::absolute` of `--project`. A relative `.uproject` handed to the editor itself is resolved from `Engine/Binaries/<platform>/`, not from the shell. Pass an absolute path on the command line anyway. That is the path the editor records, and it is the path that shows up in failures.

## Launch line

After the region is created and `InitRegion` has returned, the child is:

```
UnrealEditor-Cmd <project> <map> -game -nullrhi -unattended -nosplash -nopause -stdout -ShmName=<logical name>
```

`-game` is required. The plugin becomes ready only for a Game or PIE world, and commandlets exit on their own. `-nullrhi` disables the renderer. `-stdout` puts the engine log on the tool's terminal. The logical name is the one just created. The editor path and the project path are absolute.

## Polling

The tool sleeps 10 ms between checks. Every check, including the wait for `ue_ready` and the wait for process exit, relaxed-increments `tool_heartbeat`.

Two budgets, both fatal when exceeded:

| Wait | Budget | Failure text |
|---|---|---|
| `state == 1`, or process exit after `QUIT` | 60 s | `ready timeout` or `editor hung` |
| one ring push, or one reply with the expected id | 10 s | the command's own failure line |

If the child exits during a wait, the tool prints `editor exited <code>` and returns 1. A push that cannot find room retries until the reply budget expires. A popped reply with the wrong id is ignored and the wait continues.

Ids start at 1 and increase by 1 for each request, including `QUIT`.

## Commands that run today

`launch` prints the logical name, flushes stdout, and polls until the child exits. It sends nothing. Exit 0 only if the child exits 0.

`ping` sends nonce `0x55aa55aa55aa55aa`. It requires `PONG` with that nonce and a payload of 8 bytes. It prints `pong <hex>` and then `QUIT`.

`exec` sends the command text as `CONSOLE`. The text must fit in 1024 bytes. It requires `ACK` with `u32` value 1, prints `ok 1`, and then `QUIT`. Value 0 is `exec rejected`.

`demo` runs, in order:

1. `PING` with the same nonce as `ping`. A mismatch is `ping nonce`.
2. `CONSOLE` `stat fps`. Anything other than `ACK` value 1 fails.
3. `SPAWN` with count 1000 and spacing 100. `spawned` must be 1000.
4. `DUMP`. `DONE.bytes` must be `4 + 1000 * 12`, and that must fit in the engine half. The blob's count must be 1000. Actor `i` must satisfy `|x - i * 100| <= 0.01` and `y == z == 0`. `demo` does not write the tool half.
5. `QUIT`.

It prints `spawned 1000`. The position check is the acceptance test for the bulk fence: the tool reads the engine's bytes only after `DONE`.

`QUIT` requires an `ACK`. The tool then waits up to 60 s for exit code 0.

## Cleanup

The session owns the region and the child. If the process returns while the child is still alive, the destructor `SIGKILL`s it (TerminateProcess on Windows) and waits. It then release-stores `state = 2`. The region destructor unmaps and, on POSIX, unlinks the object. Windows drops the mapping when this last handle closes.

A failed ready wait, a failed reply wait, or a crashed child takes the same path: the error is printed, `main` returns 1, and the destructor cleans up. A POSIX crash can still leave a name behind if the process is killed from outside. The next `Create` of that same name unlinks it first. Names are random, so the stale object is reclaimed when that name is reused, not when a later run picks a new one.

## Version 2 commands

Not in the binary. Specified here so the flags have one meaning when they land. Layout of `STEP`, the halves, and `flags` is `docs/protocol.md`.

`step` sets sync (`flags` bit 0) and is the command that exercises it:

```
uectl step --dt <seconds> --count <n> --engine <root> --project <uproject> [--window]
```

Acceptance values are `--dt 0.1 --count 10`. After `ue_ready` it spawns 1000 actors at spacing 100, writes `100.f` at the first four bytes of the tool half, sends `count` steps of `dt`, checks each `STEP_ACK` (`frame` and `speed`), `DUMP`s, and checks positions against `i * 100 + count * speed * dt` within 0.01. It prints `stepped <count>`, sends `QUIT`, and uses the same 60 s exit wait as `demo`. `dt` outside `(0, 1]` fails in the plugin with `ERR` and the tool fails the step.

`--sync` is an extra flag on the existing subcommands. Without it, `ping`, `exec`, `demo`, and `launch` leave sync clear and stay on the free-running clock. With it, those commands still get `PING`, `CONSOLE`, and `QUIT` replies from the idle drain. They do not have to send `STEP` first.

`--window` is an extra flag on every subcommand. It removes `-nullrhi` from the launch line and leaves the rest, including `-game`. It also forces `t.IdleWhenNotForeground 0`, whose default is already 0. Headless `demo` does not pass `--window` and does not set a mesh on the spawned actors.

`demo` does not set sync and does not write the tool half. Its spawn count, spacing, and position check stay as they are.
