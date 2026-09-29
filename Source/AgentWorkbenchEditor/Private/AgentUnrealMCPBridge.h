#pragma once

#include "CoreMinimal.h"
#include "MCPToolTypes.h"

/** Adapts the enabled UE5.8 ToolsetRegistry and independent MCP tools. Game thread only. */
class FAgentUnrealMCPBridge
{
public:
    /** The complete local catalog. The caller decides how many definitions to send to a model. */
    static TArray<FMCPToolDefinition> ListAvailable();

    /** Unknown tools and tools without an explicit read-only classification require approval. */
    static bool RequiresApproval(const FString& RegistryName, const FString& FrozenHandlerId);

    /** Local metadata only; these never dispatch an engine tool. */
    static FMCPToolResult SearchCatalog(const TMap<FString, FString>& FrozenHandlerIds,
        const FString& Query, int32 Offset, int32 Limit);
    static FMCPToolResult DescribeCatalogTool(const TMap<FString, FString>& FrozenHandlerIds,
        const FString& RegistryName);
    static bool IsCurrentBinding(const FString& RegistryName, const FString& FrozenHandlerId);
    /** Validates a frozen target's arguments against its current UE MCP input schema. */
    static bool ValidateArguments(const FString& RegistryName, const FString& FrozenHandlerId,
        const TSharedRef<FJsonObject>& Arguments, FString& OutError);
    static FString DisplayName(const FString& RegistryName, const FString& FrozenHandlerId);

    /** Rechecks the live tool instance and schema against FrozenHandlerId before dispatch. */
    static void Execute(const FString& RegistryName, const FString& FrozenHandlerId,
        const TSharedRef<FJsonObject>& Arguments, double TimeoutSeconds, bool bApproved,
        TFunction<void(FMCPToolResult)> Completion);

    static bool IsUnrealToolName(const FString& RegistryName);
};
