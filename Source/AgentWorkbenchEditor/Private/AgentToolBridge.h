#pragma once

#include "CoreMinimal.h"
#include "MCPToolTypes.h"

struct FAgentRunInputSnapshot;

/** Run-scoped policy and provider naming across BSHarness and UE MCP tool sources. */
class FAgentToolBridge
{
public:
    // Game thread only. Enumerates approved BSHarness handlers and UE MCP
    // catalog/call tools. Individual UE schemas are fetched from the catalog.
    // Asset opening is omitted when this run has no eligible candidate;
    // read-only asset inspection remains available after an assets.search call.
    static TArray<FMCPToolDefinition> ListAvailable(const FAgentRunInputSnapshot& Snapshot);

    // Freeze the complete local catalog, including UE tools not directly sent to the model.
    // Caller sets bToolListFrozen after this returns.
    static void FreezeAvailableTools(FAgentRunInputSnapshot& Snapshot);

    // Resolve ue_mcp.call_tool to its actual target and arguments before approval.
    // Returns false with OutError for an unfrozen, missing or changed target.
    static bool ResolveApprovalTarget(const FAgentRunInputSnapshot& Snapshot,
        const FString& RegistryName, const TSharedRef<FJsonObject>& Arguments,
        FString& OutTargetRegistryName, TSharedPtr<FJsonObject>& OutTargetArguments,
        FString& OutError);
    static FString GetDisplayName(const FAgentRunInputSnapshot& Snapshot, const FString& RegistryName);

    // Game thread only. Completion is delivered on the game thread, including for async tools.
    // The current binding, frozen handler identity, approval and path policy are checked before dispatch.
    static bool RequiresApproval(const FAgentRunInputSnapshot& Snapshot, const FString& RegistryName);
    static void Execute(const FAgentRunInputSnapshot& Snapshot, const FString& RegistryName,
        const TSharedRef<FJsonObject>& Arguments, double TimeoutSeconds, bool bApproved,
        TFunction<void(FMCPToolResult)> Completion);

    // Deterministic provider-safe aliases; collisions are omitted from ListAvailable.
    // FromModelName resolves the current registry only. Model responses must use
    // the alias table captured with that request, not a later registry lookup.
    static FString ToModelName(const FString& RegistryName);
    static FString FromModelName(const FString& ModelName);
};
