# uectl

`uectl` is a C++17 process. It creates the shared region, publishes the header, starts Unreal, and polls. The layout it publishes is `docs/protocol.md`. This file owns the commands, the launch line, the timeouts, and cleanup.

The binary is `./build/uectl`, or `build\Release\uectl.exe` on Windows.

## Arguments

```
uectl launch|ping|demo --engine <root> --project <uproject> [--map <map>] [--sync] [--window]
uectl step --dt <seconds> --count <n> --engine <root> --project <uproject> [--map <map>] [--window]
uectl exec <command> --engine <root> --project <uproject> [--map <map>] [--sync] [--window]
```

`--engine` and `--project` are required. `--map` defaults to `/Engine/Maps/Entry`. `exec` joins its remaining non-option arguments with spaces and sends that string. A missing command, an unknown subcommand, a stray argument, or `step` without both `--dt` and `--count` prints usage and exits 1. `--count` has to be at least 1. `--window` is accepted on every subcommand.

`--engine` is the directory that contains one of these files. A headless launch uses the first that exists:

- `Engine/Binaries/Mac/UnrealEditor-Cmd`
- `Engine/Binaries/Win64/UnrealEditor-Cmd.exe`
- `Engine/Binaries/Linux/UnrealEditor-Cmd`

On macOS, `--window` tries `Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor` first and uses `UnrealEditor-Cmd` only if that file is missing. `UnrealEditor-Cmd` cannot become the key process, so its window does not come forward. The app bundle can.

The project path passed to the editor is `std::filesystem::absolute` of `--project`. A relative `.uproject` handed to the editor itself is resolved from `Engine/Binaries/<platform>/`, not from the shell. Pass an absolute path on the command line anyway. That is the path the editor records, and it is the path that shows up in failures.

## Launch line

After the region is created and `InitRegion` has returned, the child is:

```
UnrealEditor-Cmd <project> <map> -game -nullrhi -unattended -nosplash -nopause -stdout -ExecCmds=t.IdleWhenNotForeground 0 -ShmName=<logical name>
```

`--window` removes `-nullrhi` and `-unattended`, and leaves `-game`. Headless `demo` and headless `step` do not pass `--window`, and they keep both flags. A macOS `UnrealEditor-Cmd` window did not come forward with `-unattended` or without it. The app bundle is the launch that can become key.

`-game` is required. The plugin becomes ready only for a Game or PIE world, and commandlets exit on their own. `-nullrhi` disables the renderer. `-stdout` puts the engine log on the tool's terminal. The logical name is the one just created. The editor path and the project path are absolute.

Every launch passes `-ExecCmds=t.IdleWhenNotForeground 0`. The cvar is cheat, so this is an exec and not a project ini. What a value of 1 would skip is `docs/plugin.md`.

Sync does not change this line. `uectl step` sets `flags` bit 0 before `magic` is published. `ping`, `exec`, `demo`, and `launch` leave it clear unless `--sync` is passed.

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

`step` sets sync and is the command that exercises it. Acceptance values are `--dt 0.1 --count 10`. After `ue_ready` it:

1. `SPAWN` with count 1000 and spacing 100. `spawned` must be 1000.
2. Writes `100.f` at the first four bytes of the tool half.
3. Sends `--count` steps of `--dt`. Each reply must be `STEP_ACK` whose `frame` is the step index starting at 1, whose `speed` is `100.f`, and whose `frame` matches the header. `dt <= 0` or `dt > 1` comes back as `ERR` and the tool prints `step failed`.
4. `DUMP`. The byte count and the actor count match `demo`. Actor `i` must satisfy `|x - (i * 100 + count * speed * dt)| <= 0.01` and `y == z == 0`.
5. Prints `stepped <count>`, then `QUIT`.

A bad frame prints `step frame`. A bad speed prints `step speed`. Spawn, dump, quit, and exit failures use the same lines as `demo`.

`--window` on `step` sends the same orders with the renderer left on. Between steps the picture is still. A step moves the cubes by `speed * dt`. `launch --window` sends nothing and shows the idle world until the process exits. The HUD and the cube are `docs/plugin.md`.

`--sync` on `ping`, `exec`, `demo`, and `launch` sets the same bit and does not send `STEP`. Those commands still get their replies from the idle drain. `demo --sync` still expects the spawn positions, because nothing stepped. `demo` without `--sync` does not set the bit and does not write the tool half.

`QUIT` requires an `ACK`. The tool then waits up to 60 s for exit code 0.

## Cleanup

The session owns the region and the child. If the process returns while the child is still alive, the destructor `SIGKILL`s it (TerminateProcess on Windows) and waits. It then release-stores `state = 2`. The region destructor unmaps and, on POSIX, unlinks the object. Windows drops the mapping when this last handle closes.

A failed ready wait, a failed reply wait, or a crashed child takes the same path: the error is printed, `main` returns 1, and the destructor cleans up. A POSIX crash can still leave a name behind if the process is killed from outside. The next `Create` of that same name unlinks it first. Names are random, so the stale object is reclaimed when that name is reused, not when a later run picks a new one.
