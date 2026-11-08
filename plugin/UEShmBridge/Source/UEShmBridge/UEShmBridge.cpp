#include "Containers/Ticker.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogUEShmBridge, Log, All);

class FUEShmBridgeModule : public IModuleInterface
{
public:
	void StartupModule() override
	{
		Ticker = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateRaw(this, &FUEShmBridgeModule::Tick));
	}

	void ShutdownModule() override
	{
		FTSTicker::RemoveTicker(Ticker);
	}

private:
	bool Tick(float)
	{
		if (!bLogged)
		{
			bLogged = true;
			UE_LOG(LogUEShmBridge, Display, TEXT("ueshm tick"));
		}
		return true;
	}

	FTSTicker::FDelegateHandle Ticker;
	bool bLogged = false;
};

IMPLEMENT_MODULE(FUEShmBridgeModule, UEShmBridge)
