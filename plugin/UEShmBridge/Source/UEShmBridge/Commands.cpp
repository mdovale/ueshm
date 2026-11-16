#include "Commands.h"

#include "CoreMinimal.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"

namespace {

TArray<AStaticMeshActor*> Spawned;

// Game thread only. The drain, the end-of-frame callback, and DrawHUD do not run together.
struct Note {
	std::uint16_t type = 0;
	std::uint64_t id = 0;
	const char* result = nullptr;
};

Note Transcript[kTranscriptCap];
int TranscriptCount = 0;

float LastCompletedDt = 0.f;
bool bStepCompleted = false;

// Loop intervals kept for the render-rate average. About one second at the 30 Hz idle period, so the line follows a change.
constexpr int kRenderSamples = 32;
double RenderSamples[kRenderSamples]{};
int RenderCount = 0;
double RenderSum = 0.0;
double LastLoopSeconds = 0.0;

UStaticMesh* CachedCube = nullptr;
bool bCubeTried = false;

// The engine cube, or null when this process cannot render or the asset is missing.
// Headless does not call LoadObject. A second call returns the first result.
UStaticMesh* WindowCube() {
	if (!FApp::CanEverRender()) {
		return nullptr;
	}
	if (!bCubeTried) {
		bCubeTried = true;
		CachedCube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (CachedCube == nullptr) {
			UE_LOG(LogTemp, Error, TEXT("ueshm cube missing"));
		}
	}
	return CachedCube;
}

// Park the pawn off the row once. Actor 0's cube is centered on the origin and fills the default eye point.
void FrameWindow(UWorld* world) {
	if (world == nullptr) {
		return;
	}
	APlayerController* controller = world->GetFirstPlayerController();
	if (controller == nullptr) {
		return;
	}
	const FVector where(-400.0, -600.0, 250.0);
	const FVector at(200.0, 0.0, 0.0);
	const FRotator look = (at - where).Rotation();
	if (APawn* pawn = controller->GetPawn()) {
		pawn->SetActorLocation(where);
		pawn->SetActorRotation(look);
	}
	controller->SetControlRotation(look);
}

}  // namespace

// First Game or PIE world, or null. Editor worlds do not count.
UWorld* FindGameWorld() {
	if (!GEngine) {
		return nullptr;
	}
	for (const FWorldContext& context : GEngine->GetWorldContexts()) {
		UWorld* world = context.World();
		if (world && (world->WorldType == EWorldType::Game || world->WorldType == EWorldType::PIE)) {
			return world;
		}
	}
	return nullptr;
}

// Encode and push. False if the ring refuses the record.
// Stores last_rsp_* before the push when `header` is set.
bool PublishReply(Ring& replies, const Message& reply, Header* header) {
	if (header != nullptr) {
		header->last_rsp_type = reply.type;
		header->last_rsp_id = static_cast<std::uint32_t>(reply.id);
	}
	unsigned char buf[16 + kMaxPayload + 8];
	const std::uint32_t occ = Encode(reply, buf, sizeof(buf));
	if (occ == 0 || !replies.Push(buf, occ)) {
		UE_LOG(LogTemp, Error, TEXT("ueshm reply push failed"));
		return false;
	}
	return true;
}

// Copies the newest lines into `out`, oldest first. Returns how many were written.
int CopyTranscript(TranscriptLine* out, int cap) {
	if (out == nullptr || cap <= 0 || TranscriptCount <= 0) {
		return 0;
	}
	const int have = TranscriptCount < kTranscriptCap ? TranscriptCount : kTranscriptCap;
	const int n = have < cap ? have : cap;
	const int start = TranscriptCount - n;
	for (int i = 0; i < n; ++i) {
		const Note& slot = Transcript[(start + i) % kTranscriptCap];
		out[i].type = slot.type;
		out[i].id = slot.id;
		out[i].result = slot.result;
	}
	return n;
}

// Records one handled command. Keeps `kTranscriptCap`. `result` is ok, spawned, frame, or err.
void NoteCommand(std::uint16_t type, std::uint64_t id, const char* result) {
	Note& slot = Transcript[TranscriptCount % kTranscriptCap];
	slot.type = type;
	slot.id = id;
	slot.result = result != nullptr ? result : "err";
	++TranscriptCount;
}

// The dt of a STEP whose actors have already moved. Sim rate stays 0 until this runs.
void NoteCompletedStep(float dt) {
	LastCompletedDt = dt;
	bStepCompleted = true;
}

// Samples the wall-clock time since the previous call. The first call only sets the start.
void NoteRenderDelta() {
	// Not FApp's delta. The sync clock writes 0 on idle loops and the step dt otherwise, and neither is a present.
	const double now = FPlatformTime::Seconds();
	const double dt = LastLoopSeconds > 0.0 ? now - LastLoopSeconds : 0.0;
	LastLoopSeconds = now;
	if (!(dt > 0.0)) {
		return;
	}
	const double sample = 1.0 / dt;
	const int slot = RenderCount % kRenderSamples;
	if (RenderCount >= kRenderSamples) {
		RenderSum -= RenderSamples[slot];
	}
	RenderSamples[slot] = sample;
	RenderSum += sample;
	++RenderCount;
}

// Average of the samples NoteRenderDelta kept. 0 if none.
float RenderRate() {
	const int n = RenderCount < kRenderSamples ? RenderCount : kRenderSamples;
	if (n == 0) {
		return 0.f;
	}
	return static_cast<float>(RenderSum / static_cast<double>(n));
}

// 1/dt of the last completed STEP. 0 if none has completed.
float SimRate() {
	if (!bStepCompleted || !(LastCompletedDt > 0.f)) {
		return 0.f;
	}
	return 1.f / LastCompletedDt;
}

// dt of the last completed STEP, or 0.
float LastStepDt() {
	return bStepCompleted ? LastCompletedDt : 0.f;
}

// Loads the engine cube when this process can render. Headless returns without LoadObject.
void WarmWindowMesh() {
	WindowCube();
}

// Move each spawned actor by `speed * dt` on X. A zero speed leaves them in place.
void IntegrateSpawned(float speed, float dt) {
	if (speed == 0.f) {
		return;
	}
	const double delta = static_cast<double>(speed) * static_cast<double>(dt);
	for (AStaticMeshActor* actor : Spawned) {
		if (actor == nullptr) {
			continue;
		}
		const FVector loc = actor->GetActorLocation();
		actor->SetActorLocation(FVector(loc.X + delta, loc.Y, loc.Z));
	}
}

namespace {

// A reply of `type` that copies the command id. The payload length stays 0.
Message Reply(const Message& command, std::uint16_t type) {
	Message out;
	out.type = type;
	out.id = command.id;
	return out;
}

}  // namespace

// False after QUIT, once its ACK is on the reply ring.
// DUMP writes positions at `bulk` before that reply. `bulk` is the engine half.
bool DispatchCommand(
	const Message& command,
	Ring& replies,
	UWorld* world,
	unsigned char* bulk,
	std::uint32_t bulk_bytes,
	Header* header) {
	if (command.type == kPing && command.size == sizeof(PingPayload)) {
		Message out = Reply(command, kPong);
		out.size = sizeof(PingPayload);
		std::memcpy(out.payload, command.payload, sizeof(PingPayload));
		NoteCommand(command.type, command.id, "ok");
		PublishReply(replies, out, header);
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
		NoteCommand(command.type, command.id, ran ? "ok" : "err");
		PublishReply(replies, out, header);
		return true;
	}

	if (command.type == kSpawn && command.size == sizeof(SpawnPayload) && bulk != nullptr) {
		SpawnPayload request;
		std::memcpy(&request, command.payload, sizeof(request));
		const std::uint32_t fit = bulk_bytes >= 4 ? (bulk_bytes - 4) / 12 : 0;
		const std::uint32_t count = request.count < fit ? request.count : fit;
		// Destroyed, not only forgotten. A window would otherwise keep drawing the old row where STEP no longer moves it.
		for (AStaticMeshActor* actor : Spawned) {
			if (IsValid(actor)) {
				actor->Destroy();
			}
		}
		Spawned.Reset();
		FActorSpawnParameters params;
		params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		UStaticMesh* cube = WindowCube();
		if (world) {
			for (std::uint32_t i = 0; i < count; ++i) {
				AStaticMeshActor* actor = world->SpawnActor<AStaticMeshActor>(FVector::ZeroVector, FRotator::ZeroRotator, params);
				if (!actor) {
					break;
				}
				if (UStaticMeshComponent* mesh = actor->GetStaticMeshComponent()) {
					// Movable before the location. A static component ignores SetActorLocation.
					mesh->SetMobility(EComponentMobility::Movable);
					if (cube != nullptr) {
						mesh->SetStaticMesh(cube);
					}
				}
				const double x = static_cast<double>(i) * static_cast<double>(request.spacing);
				actor->SetActorLocation(FVector(x, 0.0, 0.0));
				Spawned.Add(actor);
			}
		}
		if (cube != nullptr) {
			FrameWindow(world);
		}
		Message out = Reply(command, kAck);
		out.size = sizeof(AckPayload);
		AckPayload ack;
		ack.value = static_cast<std::uint32_t>(Spawned.Num());
		std::memcpy(out.payload, &ack, sizeof(ack));
		NoteCommand(command.type, command.id, "spawned");
		PublishReply(replies, out, header);
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
			NoteCommand(command.type, command.id, "ok");
			PublishReply(replies, out, header);
			return true;
		}
	}

	if (command.type == kQuit) {
		Message out = Reply(command, kAck);
		NoteCommand(command.type, command.id, "ok");
		PublishReply(replies, out, header);
		FPlatformMisc::RequestExit(false);
		return false;
	}

	Message out = Reply(command, kErr);
	out.size = sizeof(ErrPayload);
	ErrPayload err;
	err.type = command.type;
	std::memcpy(out.payload, &err, sizeof(err));
	NoteCommand(command.type, command.id, "err");
	PublishReply(replies, out, header);
	return true;
}
