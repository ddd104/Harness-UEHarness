#pragma once

#include "CoreMinimal.h"
#include "AgentWorkbenchSettings.h"

class FAgentSession;

class FAgentModelClient
{
public:
    static FString ProviderName(EAgentWorkbenchProvider Provider);
    static bool Validate(const UAgentWorkbenchSettings& Settings, const FString& Model, FString& OutError);
    static bool Start(const TSharedRef<FAgentSession>& Session, const FGuid& RequestId, FString& OutError);
};
