#pragma once

#include "MCPToolRegistry.h"

/** JSON-driven registrations. Game-thread only; a failed reload preserves the old group. */
class BSHARNESSTOOLS_API FMCPToolConfiguration
{
public:
	bool LoadFile(const FString& Path, const TArray<FMCPToolBinding>& NativeHandlers, FMCPToolRegistry& Registry, FString& OutError);
	bool LoadJson(const FString& Json, const TArray<FMCPToolBinding>& NativeHandlers, FMCPToolRegistry& Registry, FString& OutError);
	int32 NumTools() const { return ManagedNames.Num(); }

private:
	TArray<FString> ManagedNames;
};
