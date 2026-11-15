#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineCustomTimeStep.h"
#include "ShmTimeStep.generated.h"

// Sync clock. Installed only when the header's sync bit is set, never from ini.
UCLASS()
class UESHMBRIDGE_API UShmTimeStep : public UEngineCustomTimeStep
{
	GENERATED_BODY()

public:
	// Region base. Call before SetCustomTimeStep. A null base leaves the engine clock in place.
	void Attach(void* InBase);

	virtual bool Initialize(UEngine* InEngine) override;
	virtual void Shutdown(UEngine* InEngine) override;
	// False once ue_ready, after this call has written FApp's delta. True before that so map load keeps the engine clock.
	virtual bool UpdateTimeStep(UEngine* InEngine) override;
	virtual ECustomTimeStepSynchronizationState GetSynchronizationState() const override;

private:
	void OnEndFrame();

	void* Base = nullptr;
	bool bAckOwed = false;
	float StepDt = 0.f;
	uint64 StepId = 0;
	FDelegateHandle EndFrameHandle;
};
