#include "Commands.h"

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"

#include <string>

#include "shm.h"

#if PLATFORM_WINDOWS
#include "shm_win.cpp"
#else
#include "shm_posix.cpp"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogUEShmBridge, Log, All);

namespace {

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

}  // namespace

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

	// Remove the ticker.
	void ShutdownModule() override
	{
		if (Ticker.IsValid()) {
			FTSTicker::RemoveTicker(Ticker);
		}
	}

private:
	// Bump the heartbeat, publish ue_ready once, and drain up to 64 commands. Stays registered.
	bool Tick(float)
	{
		auto* header = reinterpret_cast<Header*>(Region.Data());
		if (header->magic.load(std::memory_order_acquire) != kMagic || header->version != kVersion) {
			return true;
		}
		header->ue_heartbeat.fetch_add(1, std::memory_order_relaxed);

		UWorld* world = FindGameWorld();
		if (!bReady) {
			if (!world) {
				return true;
			}
			header->state.store(kStateUeReady, std::memory_order_release);
			bReady = true;
			UE_LOG(LogUEShmBridge, Display, TEXT("ueshm ready"));
		}
		if (!world) {
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
	bool bReady = false;
};

IMPLEMENT_MODULE(FUEShmBridgeModule, UEShmBridge)
