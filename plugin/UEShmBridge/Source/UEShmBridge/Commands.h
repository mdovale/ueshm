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

// Lines kept after a pop. The ring does not keep a record, and DrawHUD reads these.
constexpr int kTranscriptCap = 8;

struct TranscriptLine {
	std::uint16_t type = 0;
	std::uint64_t id = 0;
	const char* result = nullptr;
};

// Copies the newest lines into `out`, oldest first. Returns how many were written.
int CopyTranscript(TranscriptLine* out, int cap);

// Records one handled command. Keeps `kTranscriptCap`. `result` is ok, spawned, frame, or err.
void NoteCommand(std::uint16_t type, std::uint64_t id, const char* result);

// The dt of a STEP whose actors have already moved. Sim rate stays 0 until this runs.
void NoteCompletedStep(float dt);

// Samples 1/FApp::GetDeltaTime when that delta is positive. A 0-delta idle loop is not stored.
void NoteRenderDelta();

// Average of the samples NoteRenderDelta kept. 0 if none.
float RenderRate();

// 1/dt of the last completed STEP. 0 if none has completed.
float SimRate();

// dt of the last completed STEP, or 0.
float LastStepDt();

// Loads the engine cube when this process can render. Headless returns without LoadObject.
void WarmWindowMesh();
