# UEShmBridge

Runtime module inside the Unreal process. It opens the region the tool created and runs the orders in `docs/protocol.md`. This file owns startup, which thread pops the ring, and the version-2 timestep and HUD. It does not restate field offsets.

`UEShmBridge.uplugin` loads the module `PostEngineInit` on Win64, Mac, and Linux. Engine version is 5.8. The module depends on Core, CoreUObject, and Engine. `UEShmBridge.Build.cs` adds `common/` at the repo root to the include path. The module cpp includes `shm_posix.cpp` or `shm_win.cpp`, so Unreal Build Tool compiles that same file the tool links. One source, two builds.

The project is content-only. `project/ueshm.uproject` sets `AdditionalPluginDirectories` to `../plugin` and enables `UEShmBridge`.

## Startup

`StartupModule` reads `-ShmName=`. With no name it returns and registers no ticker. Opening the project in an editor session without that argument does nothing.

With a name, it `ShmRegion::Open`s the logical name. Failure logs `open failed` and registers no ticker. Success registers an `FTSTicker` on the core ticker.

`ShutdownModule` removes the ticker if it was registered.

## Tick

The ticker runs on the game thread, after that iteration's world tick. `LaunchEngineLoop` ticks the world inside `UGameEngine::Tick`, redraws, and only then calls `FTSTicker::Tick`. A command popped on tick N is visible to world tick N+1. Version 1 relies on that. `demo` never inspects the world until `DUMP`, which is a later command, so the one-tick delay is outside what the tool checks.

Each callback:

1. Acquire-loads `magic`. If it is not `UESH`, or `version` is not 1, returns and stays registered. The header is not published yet, or it is the wrong layout.
2. Relaxed-increments `ue_heartbeat`.
3. Looks for a `UWorld` of type Game or PIE. Editor worlds do not count.
4. On the first such world, release-stores `state = 1` and logs `ueshm ready` once.
5. If no game world is available, returns without popping.
6. Pops up to 64 commands. A failed pop ends the drain. A failed decode ends the drain. `DispatchCommand` returning false ends the drain. `QUIT` is the command that returns false, and it does so only after its `ACK` is pushed.

There is no blocking primitive. The callback always returns true, so the ticker stays registered until shutdown.

## Handlers

All of these run on the game thread, inside the drain. Replies are pushed before the handler returns, except where a handler refuses the record and falls through.

`PING` copies the 8-byte nonce into `PONG`.

`CONSOLE` copies at most 1024 payload bytes, writes a NUL, and calls `GEngine->Exec` on the game world. `ACK` carries `u32` 1 or 0.

`SPAWN` reads `u32 count` and `f32 spacing`. The count is capped with `(bulk_bytes - 4) / 12`. Previous actors are dropped. Each new actor is an `AStaticMeshActor` spawned at the origin with `AlwaysSpawn`, set movable, then moved to `(i * spacing, 0, 0)`. `ACK` carries how many were spawned. No mesh is assigned. Under `-nullrhi` there is nothing to draw, and the position still exists.

`DUMP` computes `4 + count * 12`. When that fits in `bulk_bytes`, it writes `u32 count` and then `count` triples of `f32` at `bulk_off`, then pushes `DONE` with that byte count. When it does not fit, the handler falls through to `ERR` and does not write the blob.

`QUIT` pushes `ACK` with an empty payload, calls `FPlatformMisc::RequestExit(false)`, and returns false.

Any other shape pushes `ERR` with the request type and returns true, so the drain continues.

A reply that does not fit in the reply ring logs `ueshm reply push failed`. The handler still returns true, except for `QUIT`, which returns false after the push attempt.

## Version 2 timestep and HUD

Not in the module yet. This is the behavior steps 3 and 4 add. The bytes are specified in `docs/protocol.md`.

Sync is `flags` bit 0, written by the tool before `magic`. Free-running sessions leave it clear and keep the ticker drain above, unchanged.

When the flag is set, the first tick that sees a matching `magic` and version installs a `UEngineCustomTimeStep` with `UEngine::SetCustomTimeStep`. It is not set from ini, because a free-running launch would then take the same clock. The class lives in this module. No extra module dependency is required. `UpdateTimeStep` runs on the game thread, from `UEngine::UpdateTimeAndHandleMaxTickRate`, which is before the world tick.

In sync mode that function is the only consumer of the command ring. The ticker still increments `ue_heartbeat` and still stores `state = 1`, and it does not pop.

`UpdateTimeStep` drains up to 64 commands and stops before a `STEP` once earlier commands in that batch are done. Those commands run before this frame's world tick, which removes the one-tick delay version 1 has. An accepted `STEP` sets `FApp`'s delta to the requested `dt` and returns false, meaning the engine must not replace that delta. The frame still runs: world tick, redraw, core ticker, then `FCoreDelegates::OnEndFrame`.

`OnEndFrame` is where an accepted `STEP` integrates actors, release-stores `frame`, and pushes `STEP_ACK`. Integration is `SetActorLocation`, not a physics step. Each spawned actor moves on X by `speed * dt`, where `speed` is the `f32` at the start of the tool half. A speed of 0 leaves them in place. Idle loops do not move them.

A loop with no `STEP` sleeps in 2 ms slices for up to one 30 Hz period, rechecking the ring between slices, then pauses the world for that tick and uses a zero delta. `PING`, `CONSOLE`, and `QUIT` are still drained on those loops. A `STEP` that arrives during the sleep ends the sleep. `UpdateTimeStep` does not block on the tool. The tool blocks on `STEP_ACK`. Reversing that deadlocks, because the ack is produced by the tick the wait would be holding.

`dt <= 0` or `dt > 1` is `ERR` from the drain, and the world is not stepped for that record.

`t.IdleWhenNotForeground` is pinned to 0. The default is already 0. If it were 1, an unfocused `-game` process would skip the world tick after `UpdateTimeStep` had already consumed `STEP`, and an ack on `OnEndFrame` would report a tick that did not happen.

The HUD is a `UCLASS` `AHUD` in this module, drawn with `UCanvas` and `GEngine->GetSmallFont()`. No UMG asset. On the first ready tick the module calls `ClientSetHUD` on the world's first player controller. `/Engine/Maps/Entry` starts one. A null controller is retried on the next tick.

`DrawHUD` reads `state`, both heartbeats, `flags`, `frame`, the last `STEP` dt, and the header's last command and reply mirror. Those are the shared-memory stats. It also prints the eight most recent commands from a buffer in the plugin. The rings do not keep a record after pop, so that scrollback is not in the region. Render rate is the recent average of `1 / FApp::GetDeltaTime()` over frames whose delta is greater than 0. Sim rate is `1 / dt` of the last accepted `STEP`, or 0 if none has completed. They are separate numbers. `frame` on screen is the last completed step, so it lags the in-progress frame by one present.

`--window` is the only path that assigns `/Engine/BasicShapes/Cube` to a spawned mesh, and it still sets the component movable before `SetActorLocation`. Headless spawns stay mesh-less. `-nullrhi` does not draw the HUD.
