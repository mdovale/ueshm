#include "ShmTimeStep.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ShmTimeStep)

#include "Commands.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"

#include <cstring>

namespace {

// One batch. A STEP ends it early. Records after that STEP wait for the next loop.
constexpr int32 kDrainCap = 64;
// Idle loops wait at most one 30 Hz period. The tool's reply budget is 10 s.
constexpr double kIdlePeriodSeconds = 1.0 / 30.0;
// A STEP that arrives between slices still fits inside the reply budget.
constexpr float kIdleSliceSeconds = 0.002f;

enum class DrainStop { Idle, Step, Quit };

// Pause through the world. Idle mode would skip the tick a later HUD still has to draw.
void SetWorldPaused(UWorld* world, bool paused) {
	if (world == nullptr) {
		return;
	}
	APlayerController* controller = world->GetFirstPlayerController();
	if (controller == nullptr) {
		return;
	}
	controller->SetPause(paused);
	if (world->IsPaused() == paused) {
		return;
	}
	if (AWorldSettings* settings = world->GetWorldSettings()) {
		settings->SetPauserPlayerState(paused ? controller->PlayerState : nullptr);
	}
}

}  // namespace

// Region base. Call before SetCustomTimeStep. A null base leaves the engine clock in place.
void UShmTimeStep::Attach(void* InBase) {
	Base = InBase;
}

// Hook the end-of-frame callback. False without a base, and SetCustomTimeStep reports that.
bool UShmTimeStep::Initialize(UEngine* /*InEngine*/) {
	if (Base == nullptr) {
		return false;
	}
	if (!EndFrameHandle.IsValid()) {
		EndFrameHandle = FCoreDelegates::OnEndFrame.AddUObject(this, &UShmTimeStep::OnEndFrame);
	}
	return true;
}

// Drop the end-of-frame hook. An ack still owed is not sent.
void UShmTimeStep::Shutdown(UEngine* /*InEngine*/) {
	if (EndFrameHandle.IsValid()) {
		FCoreDelegates::OnEndFrame.Remove(EndFrameHandle);
		EndFrameHandle.Reset();
	}
}

// Closed without a base, Synchronized otherwise. The tool, not a genlock source, sets the pace.
ECustomTimeStepSynchronizationState UShmTimeStep::GetSynchronizationState() const {
	if (Base == nullptr) {
		return ECustomTimeStepSynchronizationState::Closed;
	}
	return ECustomTimeStepSynchronizationState::Synchronized;
}

// False once ue_ready, after this call has written FApp's delta. True before that so map load keeps the engine clock.
bool UShmTimeStep::UpdateTimeStep(UEngine* /*InEngine*/) {
	if (Base == nullptr) {
		return true;
	}
	auto* header = reinterpret_cast<Header*>(Base);
	// Map load still needs the engine clock. The tool sends nothing until ue_ready.
	if (header->state.load(std::memory_order_acquire) != kStateUeReady) {
		return true;
	}

	auto drain = [this, header]() -> DrainStop {
		auto* bytes = static_cast<unsigned char*>(Base);
		Ring commands(bytes + header->cmd_off);
		Ring replies(bytes + header->rsp_off);
		UWorld* world = FindGameWorld();
		for (int32 i = 0; i < kDrainCap; ++i) {
			unsigned char buf[16 + kMaxPayload];
			std::uint32_t size = 0;
			if (!commands.Pop(buf, sizeof(buf), &size)) {
				return DrainStop::Idle;
			}
			Message command;
			if (!Decode(buf, size, &command)) {
				return DrainStop::Idle;
			}
			header->last_cmd_type = command.type;
			header->last_cmd_id = static_cast<std::uint32_t>(command.id);
			if (command.type == kStep) {
				StepPayload request;
				bool accepted = false;
				if (command.size == sizeof(request)) {
					std::memcpy(&request, command.payload, sizeof(request));
					accepted = StepDtAccepted(request.dt);
				}
				if (!accepted) {
					Message err;
					err.type = kErr;
					err.id = command.id;
					err.size = sizeof(ErrPayload);
					ErrPayload body;
					body.type = command.type;
					std::memcpy(err.payload, &body, sizeof(body));
					NoteCommand(command.type, command.id, "err");
					PublishReply(replies, err, header);
					return DrainStop::Step;
				}
				// The ack is owed, not published. OnEndFrame moves the actors first.
				bAckOwed = true;
				StepDt = request.dt;
				StepId = command.id;
				return DrainStop::Step;
			}
			if (!DispatchCommand(command, replies, world, bytes + header->ue_bulk_off, header->ue_bulk_bytes, header)) {
				return DrainStop::Quit;
			}
		}
		return DrainStop::Idle;
	};

	DrainStop stop = drain();
	if (stop == DrainStop::Idle) {
		// The tool blocks on STEP_ACK. This wait does not block on the tool.
		const double deadline = FPlatformTime::Seconds() + kIdlePeriodSeconds;
		while (FPlatformTime::Seconds() < deadline) {
			FPlatformProcess::Sleep(kIdleSliceSeconds);
			stop = drain();
			if (stop != DrainStop::Idle) {
				break;
			}
		}
	}

	UEngineCustomTimeStep::UpdateApplicationLastTime();
	if (stop == DrainStop::Step && bAckOwed) {
		FApp::SetDeltaTime(StepDt);
		FApp::SetCurrentTime(FApp::GetLastTime() + static_cast<double>(StepDt));
		SetWorldPaused(FindGameWorld(), false);
		return false;
	}

	FApp::SetDeltaTime(0.0);
	FApp::SetCurrentTime(FApp::GetLastTime());
	if (stop != DrainStop::Quit) {
		SetWorldPaused(FindGameWorld(), true);
	}
	return false;
}

// Only for a STEP consumed this loop: move the actors, release-store frame, push STEP_ACK.
void UShmTimeStep::OnEndFrame() {
	if (!bAckOwed || Base == nullptr) {
		return;
	}
	const float dt = StepDt;
	const uint64 id = StepId;
	bAckOwed = false;

	auto* header = reinterpret_cast<Header*>(Base);
	auto* bytes = static_cast<unsigned char*>(Base);
	float speed = 0.f;
	// Tool half. The command-ring acquire in the drain already published this write.
	if (header->tool_bulk_bytes >= sizeof(speed)) {
		std::memcpy(&speed, bytes + header->tool_bulk_off, sizeof(speed));
	}
	IntegrateSpawned(speed, dt);
	// Publish the count only after the move. A reader that sees it is past this step.
	const std::uint32_t frame = header->frame.load(std::memory_order_relaxed) + 1;
	header->frame.store(frame, std::memory_order_release);

	Message reply;
	reply.type = kStepAck;
	reply.id = id;
	reply.size = sizeof(StepAckPayload);
	StepAckPayload body;
	body.frame = frame;
	body.speed = speed;
	std::memcpy(reply.payload, &body, sizeof(body));
	// After the move. The transcript word is the completed step, not the pop.
	NoteCompletedStep(dt);
	NoteCommand(kStep, id, "frame");
	Ring replies(bytes + header->rsp_off);
	PublishReply(replies, reply, header);
}
