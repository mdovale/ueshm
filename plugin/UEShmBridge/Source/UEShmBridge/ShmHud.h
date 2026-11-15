#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "ShmHud.generated.h"

// Game-thread HUD. Installed with ClientSetHUD. Not a UMG widget.
UCLASS(NotPlaceable, Transient)
class UESHMBRIDGE_API AShmHud : public AHUD
{
	GENERATED_BODY()

public:
	// Region base. Call after ClientSetHUD. A null base leaves DrawHUD blank.
	void Attach(void* InBase);

	// Reads the header and the local transcript. Does not pop a ring.
	virtual void DrawHUD() override;

private:
	void* Base = nullptr;
};
