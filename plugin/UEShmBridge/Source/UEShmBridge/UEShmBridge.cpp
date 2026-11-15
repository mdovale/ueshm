#include "Commands.h"
#include "ShmTimeStep.h"

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"
#include "UObject/WeakObjectPtr.h"

#include <string>

#include "shm.h"

#if PLATFORM_WINDOWS
#include "shm_win.cpp"
#else
#include "shm_posix.cpp"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogUEShmBridge, Log, All);

class FUEShmBridgeModule : public IModuleInterface
{
public:
	// Open -ShmName and register the ticker. No name, or a failed open, registers nothing.
	void StartupModule() override
	{
		FString value;
		if (!FParse::Value(FCommandLine::Get(), TEXT("-ShmName="), value)) {
			return;
		}
		const auto converted = StringCast<ANSICHAR>(*value);
		Region = ShmRegion::Open(std::string(converted.Get(), static_cast<std::size_t>(converted.Length())));
		if (!Region) {
			UE_LOG(LogUEShmBridge, Error, TEXT("open failed"));
			return;
		}
		Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FUEShmBridgeModule::Tick));
	}

	// Remove the ticker. Drop the sync clock if this session installed one.
	void ShutdownModule() override
	{
		if (Ticker.IsValid()) {
			FTSTicker::RemoveTicker(Ticker);
		}
		// The engine may already have destroyed the clock. Get() is null then.
		if (UShmTimeStep* step = TimeStep.Get()) {
			step->RemoveFromRoot();
			if (GEngine != nullptr) {
				GEngine->SetCustomTimeStep(nullptr);
			}
		}
		TimeStep.Reset();
	}

private:
	// Bump the heartbeat and publish ue_ready once. Stays registered.
	// Sync mode does not pop. The custom timestep is the only consumer then.
	bool Tick(float)
	{
		auto* header = reinterpret_cast<Header*>(Region.Data());
		if (header->magic.load(std::memory_order_acquire) != kMagic || header->version != kVersion) {
			return true;
		}
		header->ue_heartbeat.fetch_add(1, std::memory_order_relaxed);

		// Published with magic. A free-running session leaves the bit clear and never installs this.
		const bool bSync = (header->flags & kFlagSync) != 0;
		if (bSync && !TimeStep.IsValid() && GEngine != nullptr) {
			UShmTimeStep* created = NewObject<UShmTimeStep>(GEngine);
			created->AddToRoot();
			created->Attach(Region.Data());
			TimeStep = created;
			if (!GEngine->SetCustomTimeStep(created)) {
				UE_LOG(LogUEShmBridge, Error, TEXT("timestep init failed"));
			}
		}

		UWorld* world = FindGameWorld();
		if (!bReady) {
			if (!world) {
				return true;
			}
			header->state.store(kStateUeReady, std::memory_order_release);
			bReady = true;
			UE_LOG(LogUEShmBridge, Display, TEXT("ueshm ready"));
		}
		if (!world || bSync) {
			return true;
		}

		auto* base = static_cast<unsigned char*>(Region.Data());
		Ring commands(base + header->cmd_off);
		Ring replies(base + header->rsp_off);
		for (int i = 0; i < 64; ++i) {
			unsigned char buf[16 + kMaxPayload];
			std::uint32_t size = 0;
			if (!commands.Pop(buf, sizeof(buf), &size)) {
				break;
			}
			Message command;
			if (!Decode(buf, size, &command)) {
				break;
			}
			// Low 32 bits. The ring id stays 64-bit; this mirror is what a later HUD can read.
			header->last_cmd_type = command.type;
			header->last_cmd_id = static_cast<std::uint32_t>(command.id);
			if (!DispatchCommand(
					command, replies, world, base + header->ue_bulk_off, header->ue_bulk_bytes, header)) {
				break;
			}
		}
		return true;
	}

	ShmRegion Region;
	FTSTicker::FDelegateHandle Ticker;
	TWeakObjectPtr<UShmTimeStep> TimeStep;
	bool bReady = false;
};

IMPLEMENT_MODULE(FUEShmBridgeModule, UEShmBridge)
