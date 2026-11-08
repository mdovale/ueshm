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

	void ShutdownModule() override
	{
		if (Ticker.IsValid()) {
			FTSTicker::RemoveTicker(Ticker);
		}
	}

private:
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
			if (!DispatchCommand(command, replies, world, base + header->bulk_off, header->bulk_bytes)) {
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
