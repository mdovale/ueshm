#pragma once

#include "protocol.h"

class UWorld;

// False after QUIT, once its ACK is on the reply ring.
bool DispatchCommand(const Message& command, Ring& replies, UWorld* world);
