#pragma once

#include "AgentWorkbenchSession.h"
#include "AssetRegistry/AssetData.h"
#include "UObject/SoftObjectPath.h"

class FJsonObject;

struct FAgentAddCandidatesResult
{
    int32 Added = 0;
    int32 Duplicates = 0;
    int32 Skipped = 0;
    int32 InvalidPath = 0;
    int32 UnsupportedType = 0;
    int32 UnknownType = 0;
    int32 OverLimit = 0;
    FString Message;
};

struct FAgentAssetLookupResult
{
    EAgentCandidateValidation Status = EAgentCandidateValidation::Unknown;
    FAssetData Data;
};

class FAgentAssetContextService
{
public:
    // Reads only the primary Content Browser at the moment the target window invokes it.
    static FAgentAddCandidatesResult AddSelectedAssets(FAgentSession& Session, int32 MaxCandidates);
    static FAgentAddCandidatesResult AddAssets(FAgentSession& Session, const TArray<FAssetData>& Assets, int32 MaxCandidates);
    static bool NormalizeObjectPath(const FString& Input, FString& OutPath);
    static FAgentAssetLookupResult LookupAsset(const FSoftObjectPath& Path);
    static bool BuildSnapshot(FAgentSession& Session, const FGuid& RunId,
        TFunctionRef<FAgentAssetLookupResult(const FSoftObjectPath&)> Lookup,
        FAgentRunInputSnapshot& OutSnapshot, FString& OutError);
};

class FAgentReadOnlyTools
{
public:
    // Only these two names are registered for P2; unknown tools and unauthorized paths return errors.
    static TSharedRef<FJsonObject> Execute(const FAgentRunInputSnapshot& Snapshot, const FString& ToolName,
        const TSharedPtr<FJsonObject>& Arguments,
        TFunctionRef<FAgentAssetLookupResult(const FSoftObjectPath&)> Lookup);
};
