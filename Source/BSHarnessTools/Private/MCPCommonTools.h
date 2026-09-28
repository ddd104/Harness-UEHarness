#pragma once

#include "MCPToolRegistry.h"

/** Native handler catalog, independent of exposed tool names. Owner drains workers on shutdown. */
TArray<FMCPToolBinding> CreateMCPNativeHandlers(TArray<TFuture<void>>& BackgroundTasks);

/** Validates the flat parameter schema supported by native handlers. */
FString ValidateMCPToolArguments(const FMCPToolArguments& Arguments, const TSharedRef<FJsonObject>& Schema, bool bCheckRequired = true);
