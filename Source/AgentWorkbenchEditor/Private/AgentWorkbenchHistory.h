#pragma once

#include "CoreMinimal.h"
#include "AgentWorkbenchSession.h"

// Project Saved data only. No FAssetData, UObject, credentials, or runtime handles are serialized.
class FAgentWorkbenchHistory
{
public:
    static FString DefaultDirectory();
    static bool Save(const FAgentSession& Session, const FString& Directory);
    static void LoadAll(const FString& Directory, TArray<TSharedPtr<FAgentSession>>& OutSessions,
        TArray<FString>& OutErrors, bool bValidateAssets = true);
};
