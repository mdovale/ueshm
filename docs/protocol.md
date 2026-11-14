# Protocol

This file owns the shared-memory layout: header, rings, records, orders, bulk, and fences. Other docs point here instead of restating it.

The binary speaks version 1. Version 2 is the contract for the rest of M1. It is specified below and is not in the tree yet. Until it lands, `kVersion` stays 1, the pad at offset 48 stays zero, and there is one bulk writer.

Both sides compile the same `common/protocol.h` and `common/ring.h`. Shared structs use fixed-width integers, explicit alignment, and little-endian stores. They contain no pointers, no STL, and no exceptions. `static_assert` pins `sizeof` and `offsetof` on both builds.

## Names

The logical name is `ueshm-` plus 8 lowercase hex digits. `uectl` draws those digits from `std::random_device`. The plugin receives the logical name as `-ShmName=`.

| Host | Object name |
|---|---|
| macOS, Linux | `/` plus the logical name |
| Windows | `Local\` plus the logical name |

macOS rejects a POSIX shared-memory name longer than 31 bytes, including the leading slash. `/ueshm-` plus 8 hex fits. A longer name fails `ShmRegion::Create`.

`ShmRegion::Create` is the tool. On POSIX it `shm_unlink`s a stale object of that name, then `shm_open`s with `O_CREAT|O_EXCL`, `ftruncate`s once, and `mmap`s. The destructor unmaps, closes, and unlinks. On Windows it `CreateFileMappingA`s a pagefile mapping. `SetLastError(0)` runs first so `ERROR_ALREADY_EXISTS` means this call found an existing name. The mapping disappears with the last handle, so the tool keeps its handle until the child has exited. `Open` reads the size with `NtQuerySection` (`SectionBasicInformation`) and falls back to `VirtualQuery`. `shm_win.cpp` is Win64 only.

`Create` returns an empty region for an empty name or a zero size. The mapping is zero-filled by the OS. `InitRegion` also `memset`s the whole mapping before it publishes the header.

## Header

128 bytes. `InitRegion` writes every field, inits both rings, then release-stores `magic`. The plugin acquire-loads `magic` and only then trusts the rest. A null base, or a mapping shorter than `kBulkOff` (131712), is rejected and `magic` stays 0.

| Offset | Field | Type | Writer | Meaning |
|---|---|---|---|---|
| 0 | `magic` | atomic `u32` | tool, last | `0x48534555` (`UESH`) once the header is published |
| 4 | `version` | `u32` | tool | `1` in this binary |
| 8 | `total_size` | `u32` | tool | mapped bytes |
| 12 | `state` | atomic `u32` | both | `0` created, `1` ue_ready, `2` closing |
| 16 | `tool_heartbeat` | atomic `u32` | tool | incremented by 1 on every poll |
| 20 | `ue_heartbeat` | atomic `u32` | plugin | incremented by 1 on every tick |
| 24 | `cmd_off` | `u32` | tool | 128. First byte of the command ring (`head`) |
| 28 | `cmd_bytes` | `u32` | tool | 65536. Data capacity, not the mapped ring span |
| 32 | `rsp_off` | `u32` | tool | 65920. First byte of the reply ring |
| 36 | `rsp_bytes` | `u32` | tool | 65536 |
| 40 | `bulk_off` | `u32` | tool | 131712. First bulk byte |
| 44 | `bulk_bytes` | `u32` | tool | `total_size - bulk_off`. 16645504 in the default 16 MiB region |
| 48 | `pad` | 80 bytes | tool | zero |

`state` moves `0 → 1` when the plugin first sees a `UWorld` of type Game or PIE, with a release store. The tool acquire-loads it. The tool stores `2` in its session destructor, including after a timeout or a crash. Heartbeats use relaxed adds. They are liveness counters, not fences.

A version other than `kVersion` never stores `state = 1`. The tool then hits the ready timeout.

Default region, widths not to scale:

```
0        128      65920    131712                         16777216
| header | cmd ring | rsp ring | bulk                                  |
```

Each ring's mapped span is 65792 bytes: 128 for `head`, 128 for `tail`, 65536 of data. `cmd_off` and `rsp_off` point at that ring's `head`.

## Rings

One producer, one consumer, in bytes. The command ring's producer is the tool and its consumer is the plugin. The reply ring is the opposite. Two consumers on one ring is a broken session.

`head` is the producer byte offset. `tail` is the consumer byte offset. Both are monotonic `uint32_t`. Bytes in use are `head - tail`, computed unsigned. The data index is `counter & 65535`. Empty means `head == tail`.

A record is never split across the end of the data. If the bytes left at the end are shorter than the record, that tail is skipped and the record is written at offset 0. The producer writes a `u32` 0 into a skipped tail when the gap is at least 4 bytes, so a consumer that looks there does not treat leftover bytes as a size. Full means the next record, including a skipped tail, would push `head - tail` past 65536.

`Push` rejects a null record, a length below 16, a length above 65536, a length that is not a multiple of 8, or a length that disagrees with `Align8` of the record's first `u32`. The producer copies the bytes, then release-stores `head`. The consumer acquire-loads `head`, copies, then release-stores `tail`.

`std::atomic<uint32_t>` is required to be lock-free and 4 bytes wide. That is static-asserted.

## Records

Each ring entry is a 16-byte prefix, the payload, then zeros out to a multiple of 8.

```
u32 size | u16 type | u16 pad | u64 id | payload | pad to 8
```

`pad` is 0 so that `id` is 8-byte aligned. `size` is 16 plus the payload length. It does not include the trailing pad. The ring advances by `Align8(size)`.

`id` is a tool counter starting at 1. Replies copy it. `uectl` discards a reply whose `id` is not the one it is waiting on, and keeps polling until the match or the reply timeout.

`Encode` returns 0 if the payload is larger than 1024 or the destination cannot hold the padded record. `Decode` rejects a short buffer, a non-zero `pad`, a `size` below 16, a `size` past the popped record, or a payload above 1024.

Local `Message` objects are not stored in the region. Only the encoded bytes are.

## Orders in version 1

| Type | Value | Payload | Reply |
|---|---|---|---|
| `PING` | 1 | `u64` nonce | `PONG` (101) plus the same nonce |
| `CONSOLE` | 2 | raw UTF-8, no NUL | `ACK` (102) plus `u32 ok` |
| `SPAWN` | 3 | `u32 count`, `f32 spacing` | `ACK` plus `u32 spawned` |
| `DUMP` | 4 | empty | `DONE` (103) plus `u32 bytes` |
| `QUIT` | 5 | empty | `ACK` with no payload |

Anything else, including a known type with the wrong payload size, is `ERR` (104). The payload is a `u32` holding the request's type. The id is the request's id.

`CONSOLE` is handed to `GEngine->Exec` on the game world. The handler adds a NUL. `ok` is 1 when `Exec` returns true, and 0 when the engine, the world, or `Exec` refuses it. `demo` sends the bytes `stat fps`.

`SPAWN` replaces any actors a previous `SPAWN` created. Actor `i` is a movable `AStaticMeshActor` with no mesh, at world position `(i * spacing, 0, 0)`, in centimeters. The actor is spawned at the origin, the mesh component is set movable, then the location is applied. A static component would ignore the location. Collision handling is `AlwaysSpawn`. The count is capped to what the bulk area can hold: `(bulk_bytes - 4) / 12`. The ack's `spawned` value is how many actors exist after the call, which can be less than `count` if a spawn returns null.

`DUMP` writes `u32 count`, then `count × 3 × f32` positions, in spawn order, at `bulk_off`. It writes the blob first, then pushes `DONE`. `DONE.bytes` equals `4 + count * 12`. A blob that does not fit in `bulk_bytes` gets `ERR` and does not write. The tool checks `|x - i * spacing| <= 0.01` and `y == z == 0` only after the matching `DONE`.

`QUIT` pushes `ACK`, then calls `FPlatformMisc::RequestExit(false)`. The handler returns after that, and the ticker stops draining for the rest of that tick.

## Bulk in version 1

The engine is the only writer. The tool reads bulk only after the matching `DONE`. There is no lock and no generation counter. The reply's release store is the fence: the plugin finishes the `memcpy` into bulk, then release-stores the reply ring's `head`, and the tool acquire-loads that `head` before it reads bulk.

## Fences

| Store | Order | What it publishes |
|---|---|---|
| `magic` | release, after the header and both rings are initialized | the header |
| `state = 1` | release | a game world exists |
| `state = 2` | release | the tool is tearing the region down |
| ring `head` | release, after the record bytes are copied | that record |
| ring `tail` | release, after the consumer has copied | the slot can be reused |
| heartbeats | relaxed | nothing |

## Version 2 contract

Not compiled in yet. Steps 2–4 of M1 implement this section. `docs/tool.md` and `docs/plugin.md` describe the command and the engine behavior that go with it. The offsets here win if another doc disagrees.

`kVersion` becomes 2. The header stays 128 bytes. Offsets 0 through 44 do not move. The new fields occupy the pad.

| Offset | Field | Type | Who writes it |
|---|---|---|---|
| 48 | `flags` | `u32` | tool, before `magic` |
| 52 | `frame` | atomic `u32` | plugin, after a completed step |
| 56 | `last_cmd_type` | `u32` | plugin, when a command is popped |
| 60 | `last_cmd_id` | `u32` | plugin. Low 32 bits of the ring id |
| 64 | `last_rsp_type` | `u32` | plugin, when a reply is pushed |
| 68 | `last_rsp_id` | `u32` | plugin. Low 32 bits of the ring id |
| 72 | `tool_bulk_off` | `u32` | tool, in `InitRegion` |
| 76 | `tool_bulk_bytes` | `u32` | tool |
| 80 | `ue_bulk_off` | `u32` | tool |
| 84 | `ue_bulk_bytes` | `u32` | tool |
| 88 | `pad` | 40 bytes | zero |

`flags` bit 0 is sync. Every other bit stays 0. The ring id on the wire stays `u64`. The header mirror keeps the low half so the HUD can read it as a `u32`.

`bulk_off` and `bulk_bytes` still name the whole bulk span. The halves are a split of that span:

- The tool half starts at `bulk_off`. Its length is `bulk_bytes / 2`, rounded down to a multiple of 8.
- The engine half starts at `tool_bulk_off + tool_bulk_bytes` and runs through the last mapped byte.

`InitRegion` writes `flags`, the four half fields, and `frame = 0` before the release store of `magic`. The plugin reads `flags` only after `magic` and `version` match.

The tool is the only writer of the tool half. The engine is the only writer of the engine half. Either side may read either half. No byte has two writers. There is still no lock, seqlock, or generation counter in the region.

`DUMP` writes its position blob at `ue_bulk_off`. The spawn cap becomes `(ue_bulk_bytes - 4) / 12`. Half of the default region still holds 1000 actors.

`last_cmd_*` and `last_rsp_*` are a HUD mirror. They are not a fence. The reply ring is still the fence for engine-half bytes. The engine reads the tool half while handling `STEP`, which is after the tool has finished writing it and pushed that record.

New order:

| Type | Value | Payload | Reply |
|---|---|---|---|
| `STEP` | 6 | `f32 dt` seconds | `STEP_ACK` (105) |

`STEP_ACK` payload is `u32 frame`, then `f32 speed`. `frame` is the value just stored in the header. `speed` is the `f32` the plugin read from the first four bytes of the tool half.

`dt <= 0` or `dt > 1` is `ERR`. The world does not tick for that record.

`STEP_ACK` is pushed from the end-of-frame callback, after the world tick, not from the drain that popped `STEP`. Every other reply is still pushed from its handler, immediately. `frame` is release-stored in that same end-of-frame callback, so a reader that sees `frame == N` is also allowed to treat step N's engine-half bytes as published. Do not publish `frame` when `STEP` is merely popped. That would make the counter visible before the tick has run.

Request type 6 stays above the version-1 request range and below the reply range. Reply type 105 stays in the reply range.
