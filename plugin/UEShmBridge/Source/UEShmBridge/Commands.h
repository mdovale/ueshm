#pragma once

#include "protocol.h"

class UWorld;

// First Game or PIE world, or null. Editor worlds do not count.
UWorld* FindGameWorld();

// False after QUIT, once its ACK is on the reply ring.
// DUMP writes positions at `bulk` before that reply. `bulk` is the engine half.
bool DispatchCommand(
	const Message& command,
	Ring& replies,
	UWorld* world,
	unsigned char* bulk,
	std::uint32_t bulk_bytes,
	Header* header);

// Encode and push. False if the ring refuses the record.
// Stores last_rsp_* before the push when `header` is set.
bool PublishReply(Ring& replies, const Message& reply, Header* header);

// Move each spawned actor by `speed * dt` on X. A zero speed leaves them in place.
void IntegrateSpawned(float speed, float dt);
