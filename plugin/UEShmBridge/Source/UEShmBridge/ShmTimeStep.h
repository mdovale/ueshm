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

	// Hook the end-of-frame callback. False without a base, and SetCustomTimeStep reports that.
	virtual bool Initialize(UEngine* InEngine) override;
	// Drop the end-of-frame hook. An ack still owed is not sent.
	virtual void Shutdown(UEngine* InEngine) override;
	// False once ue_ready, after this call has written FApp's delta. True before that so map load keeps the engine clock.
	virtual bool UpdateTimeStep(UEngine* InEngine) override;
	// Closed without a base, Synchronized otherwise. The tool, not a genlock source, sets the pace.
	virtual ECustomTimeStepSynchronizationState GetSynchronizationState() const override;

private:
	// Only for a STEP consumed this loop: move the actors, release-store frame, push STEP_ACK.
	void OnEndFrame();

	void* Base = nullptr;
	bool bAckOwed = false;
	float StepDt = 0.f;
	uint64 StepId = 0;
	FDelegateHandle EndFrameHandle;
};
