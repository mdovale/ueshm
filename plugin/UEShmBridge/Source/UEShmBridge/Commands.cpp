#include "Commands.h"

#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"

namespace {

TArray<AStaticMeshActor*> Spawned;

}  // namespace

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

bool DispatchCommand(
	const Message& command, Ring& replies, UWorld* world, unsigned char* bulk, std::uint32_t bulk_bytes) {
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

	if (command.type == kSpawn && command.size == sizeof(SpawnPayload) && bulk != nullptr) {
		SpawnPayload request;
		std::memcpy(&request, command.payload, sizeof(request));
		const std::uint32_t fit = bulk_bytes >= 4 ? (bulk_bytes - 4) / 12 : 0;
		const std::uint32_t count = request.count < fit ? request.count : fit;
		Spawned.Reset();
		FActorSpawnParameters params;
		params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		if (world) {
			for (std::uint32_t i = 0; i < count; ++i) {
				AStaticMeshActor* actor = world->SpawnActor<AStaticMeshActor>(FVector::ZeroVector, FRotator::ZeroRotator, params);
				if (!actor) {
					break;
				}
				if (UStaticMeshComponent* mesh = actor->GetStaticMeshComponent()) {
					mesh->SetMobility(EComponentMobility::Movable);
				}
				const double x = static_cast<double>(i) * static_cast<double>(request.spacing);
				actor->SetActorLocation(FVector(x, 0.0, 0.0));
				Spawned.Add(actor);
			}
		}
		Message out = Reply(command, kAck);
		out.size = sizeof(AckPayload);
		AckPayload ack;
		ack.value = static_cast<std::uint32_t>(Spawned.Num());
		std::memcpy(out.payload, &ack, sizeof(ack));
		PushReply(replies, out);
		return true;
	}

	if (command.type == kDump && command.size == 0 && bulk != nullptr) {
		const std::uint32_t count = static_cast<std::uint32_t>(Spawned.Num());
		const std::uint32_t nbytes = 4u + count * 12u;
		if (nbytes <= bulk_bytes) {
			std::memcpy(bulk, &count, sizeof(count));
			unsigned char* cursor = bulk + sizeof(count);
			for (AStaticMeshActor* actor : Spawned) {
				const FVector loc = actor->GetActorLocation();
				const float xyz[3] = {
					static_cast<float>(loc.X),
					static_cast<float>(loc.Y),
					static_cast<float>(loc.Z),
				};
				std::memcpy(cursor, xyz, sizeof(xyz));
				cursor += sizeof(xyz);
			}
			Message out = Reply(command, kDone);
			out.size = sizeof(DonePayload);
			DonePayload done;
			done.bytes = nbytes;
			std::memcpy(out.payload, &done, sizeof(done));
			PushReply(replies, out);
			return true;
		}
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
