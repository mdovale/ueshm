#pragma once

#include "protocol.h"

class UWorld;

// False after QUIT, once its ACK is on the reply ring.
// DUMP writes positions at `bulk` before that reply. `bulk` is the engine half.
bool DispatchCommand(
	const Message& command,
	Ring& replies,
	UWorld* world,
	unsigned char* bulk,
	std::uint32_t bulk_bytes,
	Header* header);
