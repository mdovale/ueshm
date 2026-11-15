# UEShmBridge

Runtime module inside the Unreal process. It opens the region the tool created and runs the orders in `docs/protocol.md`. This file owns startup, which thread pops the ring, and the version-2 timestep and HUD. It does not restate field offsets.

`UEShmBridge.uplugin` loads the module `PostEngineInit` on Win64, Mac, and Linux. Engine version is 5.8. The module depends on Core, CoreUObject, and Engine. `UEShmBridge.Build.cs` adds `common/` at the repo root to the include path. The module cpp includes `shm_posix.cpp` or `shm_win.cpp`, so Unreal Build Tool compiles that same file the tool links. One source, two builds.

The project is content-only. `project/ueshm.uproject` sets `AdditionalPluginDirectories` to `../plugin` and enables `UEShmBridge`.

## Startup

`StartupModule` reads `-ShmName=`. With no name it returns and registers no ticker. Opening the project in an editor session without that argument does nothing.

With a name, it `ShmRegion::Open`s the logical name. Failure logs `open failed` and registers no ticker. Success registers an `FTSTicker` on the core ticker.

`ShutdownModule` removes the ticker if it was registered.

## Tick

The ticker runs on the game thread. In free-running mode it runs after that iteration's world tick. `LaunchEngineLoop` ticks the world inside `UGameEngine::Tick`, redraws, and only then calls `FTSTicker::Tick`. A command popped there is visible to world tick N+1. `demo` never inspects the world until `DUMP`, which is a later command, so that delay is outside what the tool checks. Sync mode does not pop here. The custom timestep pops before the world tick.

Each callback:

1. Acquire-loads `magic`. If it is not `UESH`, or `version` is not 2, returns and stays registered. The header is not published yet, or it is the wrong layout.
2. Relaxed-increments `ue_heartbeat`.
3. If sync is set and the timestep is not installed yet, creates `UShmTimeStep` and calls `UEngine::SetCustomTimeStep`. This is not an ini class, so a free-running launch does not take the clock.
4. Looks for a `UWorld` of type Game or PIE. Editor worlds do not count.
5. On the first such world, release-stores `state = 1` and logs `ueshm ready` once.
6. If no game world is available, returns without popping.
7. If sync is set, returns without popping.
8. Otherwise pops up to 64 commands. A failed pop ends the drain. A failed decode ends the drain. `DispatchCommand` returning false ends the drain. `QUIT` is the command that returns false, and it does so only after its `ACK` is pushed.

There is no blocking primitive. The callback always returns true, so the ticker stays registered until shutdown. Shutdown also clears the custom timestep when this session installed one.

## Handlers

All of these run on the game thread, inside the drain. Replies are pushed before the handler returns, except where a handler refuses the record and falls through.

`PING` copies the 8-byte nonce into `PONG`.

`CONSOLE` copies at most 1024 payload bytes, writes a NUL, and calls `GEngine->Exec` on the game world. `ACK` carries `u32` 1 or 0.

`SPAWN` reads `u32 count` and `f32 spacing`. The count is capped with `(ue_bulk_bytes - 4) / 12`. Previous actors are dropped. Each new actor is an `AStaticMeshActor` spawned at the origin with `AlwaysSpawn`, set movable, then moved to `(i * spacing, 0, 0)`. `ACK` carries how many were spawned. No mesh is assigned. Under `-nullrhi` there is nothing to draw, and the position still exists.

`DUMP` computes `4 + count * 12`. When that fits in the engine half, it writes `u32 count` and then `count` triples of `f32` at `ue_bulk_off`, then pushes `DONE` with that byte count. When it does not fit, the handler falls through to `ERR` and does not write the blob. The handler does not store into the tool half.

On each popped command the ticker stores `last_cmd_type` and the low 32 bits of the id. Each reply stores `last_rsp_type` and the low 32 bits of the id before the reply ring is published. That mirror is not the fence.

`QUIT` pushes `ACK` with an empty payload, calls `FPlatformMisc::RequestExit(false)`, and returns false.

`STEP` is not handled in this drain. A free-running `STEP` falls through to `ERR`.

Any other shape pushes `ERR` with the request type and returns true, so the drain continues.

A reply that does not fit in the reply ring logs `ueshm reply push failed`. The handler still returns true, except for `QUIT`, which returns false after the push attempt.

## Sync clock

`UShmTimeStep` is a `UEngineCustomTimeStep` in this module. `Engine` is already a dependency. `TimeManagement` is not. The bytes are `docs/protocol.md`.

The ticker installs it on the first callback that sees a matching `magic` and version with sync set. `UpdateTimeStep` runs on the game thread, from `UEngine::UpdateTimeAndHandleMaxTickRate`, before the world tick. Until `state` is `ue_ready` it returns true and leaves the engine clock in place, so map load still advances. After that it is the only consumer of the command ring, and it returns false so the engine keeps the delta written here.

It drains up to 64 commands. Commands before a `STEP` are applied. The `STEP` is consumed. Anything after it stays for the next loop. An accepted `dt` sets `FApp`'s delta to that `dt`, advances current time by the same amount, unpauses the world, and remembers that an ack is owed. `dt <= 0` or `dt > 1` pushes `ERR` from this drain. The world is not stepped for that record.

A loop with no `STEP` sleeps in 2 ms slices for up to one 30 Hz period and rechecks the ring between slices. A `STEP` or `QUIT` that arrives during the sleep ends it. The wait does not block on the tool. The tool blocks on the reply. Reversing that deadlocks, because `STEP_ACK` is produced by the frame the wait would be holding. After the sleep, with still no accepted `STEP`, the delta is 0 and the world is paused for that tick. Pause goes through the player controller, not through `t.IdleWhenNotForeground`. A zero delta plus pause presents the frame and does not move the spawned actors.

`QUIT` pushes `ACK`, requests exit, and returns false without that idle pause.

`OnEndFrame` runs only for a `STEP` consumed this loop. It reads the `f32` at the start of the tool half, moves each spawned actor on X by `speed * dt` with `SetActorLocation`, release-stores `frame`, and pushes `STEP_ACK`. A speed of 0 leaves the actors in place. Idle loops and free-running mode do not move them. `frame` is the completed-step count. It is not stored from `UpdateTimeStep`.

The sync clock sets `t.IdleWhenNotForeground` to 0 when it installs. The default is already 0. The cvar is marked cheat, so a project ini entry is refused and only the engine's `ConsoleVariables.ini` may set it that way. If the value were 1, an unfocused `-game` process would skip the world tick after `STEP` had been consumed, and the end-of-frame ack would report a tick that did not happen.

## Not built yet

The HUD is not in the module. It will be a `UCLASS` `AHUD`, drawn with `UCanvas` and `GEngine->GetSmallFont()`. No UMG asset. On the first ready tick the module will call `ClientSetHUD` on the world's first player controller. `/Engine/Maps/Entry` starts one. A null controller is retried on the next tick.

`DrawHUD` will read `state`, both heartbeats, `flags`, `frame`, the last `STEP` dt, and the header's last command and reply mirror. It will also print the eight most recent commands from a buffer in the plugin. The rings do not keep a record after pop. Render rate is the recent average of `1 / FApp::GetDeltaTime()` over frames whose delta is greater than 0. Sim rate is `1 / dt` of the last accepted `STEP`, or 0 if none has completed. They are separate numbers. `frame` on screen is the last completed step.

`--window` is the only path that will assign `/Engine/BasicShapes/Cube` to a spawned mesh. Headless spawns stay mesh-less. `-nullrhi` does not draw a HUD.
