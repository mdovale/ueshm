#include "Commands.h"

#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"

namespace {

bool PushReply(Ring& replies, const Message& reply) {
	unsigned char buf[16 + kMaxPayload + 8];
	const std::uint32_t occ = Encode(reply, buf, sizeof(buf));
	if (occ == 0 || !replies.Push(buf, occ)) {
		UE_LOG(LogTemp, Error, TEXT("ueshm reply push failed"));
		return false;
	}
	return true;
}

Message Reply(const Message& command, std::uint16_t type) {
	Message out;
	out.type = type;
	out.id = command.id;
	return out;
}

}  // namespace

bool DispatchCommand(const Message& command, Ring& replies, UWorld* world) {
	if (command.type == kPing && command.size == sizeof(PingPayload)) {
		Message out = Reply(command, kPong);
		out.size = sizeof(PingPayload);
		std::memcpy(out.payload, command.payload, sizeof(PingPayload));
		PushReply(replies, out);
		return true;
	}

	if (command.type == kConsole) {
		char text[kMaxPayload + 1];
		const std::uint32_t n = command.size < kMaxPayload ? command.size : kMaxPayload;
		if (n != 0) {
			std::memcpy(text, command.payload, n);
		}
		text[n] = '\0';
		const bool ran = GEngine && world && GEngine->Exec(world, UTF8_TO_TCHAR(text));
		Message out = Reply(command, kAck);
		out.size = sizeof(AckPayload);
		AckPayload ack;
		ack.value = ran ? 1u : 0u;
		std::memcpy(out.payload, &ack, sizeof(ack));
		PushReply(replies, out);
		return true;
	}

	if (command.type == kQuit) {
		Message out = Reply(command, kAck);
		PushReply(replies, out);
		FPlatformMisc::RequestExit(false);
		return false;
	}

	Message out = Reply(command, kErr);
	out.size = sizeof(ErrPayload);
	ErrPayload err;
	err.type = command.type;
	std::memcpy(out.payload, &err, sizeof(err));
	PushReply(replies, out);
	return true;
}
