#include "ShmHud.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ShmHud)

#include "Commands.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"

namespace {

// Wire type as a short label. Unknown types stay numeric so the line still names the record.
FString CommandLabel(std::uint16_t type) {
	switch (type) {
	case kPing:
		return TEXT("ping");
	case kConsole:
		return TEXT("console");
	case kSpawn:
		return TEXT("spawn");
	case kDump:
		return TEXT("dump");
	case kQuit:
		return TEXT("quit");
	case kStep:
		return TEXT("step");
	default:
		return FString::Printf(TEXT("%u"), static_cast<unsigned>(type));
	}
}

}  // namespace

// Region base. Call after ClientSetHUD. A null base leaves DrawHUD blank.
void AShmHud::Attach(void* InBase) {
	Base = InBase;
}

// Reads the header and the local transcript. Does not pop a ring.
void AShmHud::DrawHUD() {
	Super::DrawHUD();
	if (Canvas == nullptr || Base == nullptr || GEngine == nullptr) {
		return;
	}
	UFont* font = GEngine->GetSmallFont();
	if (font == nullptr) {
		return;
	}

	auto* header = reinterpret_cast<Header*>(Base);
	const std::uint32_t state = header->state.load(std::memory_order_acquire);
	const std::uint32_t tool_hb = header->tool_heartbeat.load(std::memory_order_relaxed);
	const std::uint32_t ue_hb = header->ue_heartbeat.load(std::memory_order_relaxed);
	const std::uint32_t frame = header->frame.load(std::memory_order_acquire);

	float y = 16.f;
	const float x = 16.f;
	// Shadow then light text. The default view is a bright sky, and unshadowed white disappears on it.
	auto line = [&](const FString& text) {
		Canvas->SetDrawColor(0, 0, 0, 255);
		Canvas->DrawText(font, text, x + 1.f, y + 1.f);
		Canvas->SetDrawColor(240, 240, 240, 255);
		const float h = Canvas->DrawText(font, text, x, y);
		y += (h > 0.f ? h : font->GetMaxCharHeight()) + 2.f;
	};

	line(FString::Printf(TEXT("state %u"), state));
	line(FString::Printf(TEXT("tool %u"), tool_hb));
	line(FString::Printf(TEXT("ue %u"), ue_hb));
	line(FString::Printf(TEXT("flags %u"), header->flags));
	line(FString::Printf(TEXT("frame %u"), frame));
	line(FString::Printf(TEXT("last_cmd %u %u"), header->last_cmd_type, header->last_cmd_id));
	line(FString::Printf(TEXT("last_rsp %u %u"), header->last_rsp_type, header->last_rsp_id));
	line(FString::Printf(TEXT("step_dt %.3f"), LastStepDt()));
	// Separate labels. A 0-delta idle loop is not a sample of either rate.
	line(FString::Printf(TEXT("render %.1f"), RenderRate()));
	line(FString::Printf(TEXT("sim %.1f"), SimRate()));

	TranscriptLine notes[kTranscriptCap];
	const int count = CopyTranscript(notes, kTranscriptCap);
	for (int i = 0; i < count; ++i) {
		const FString label = CommandLabel(notes[i].type);
		const char* result = notes[i].result != nullptr ? notes[i].result : "";
		line(FString::Printf(
			TEXT("%s %llu %s"),
			*label,
			static_cast<unsigned long long>(notes[i].id),
			ANSI_TO_TCHAR(result)));
	}
}
