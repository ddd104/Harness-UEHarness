#pragma once

#include "MCPToolRegistry.h"

/**
 * JSON-RPC adapter for MCP tools/list and tools/call, called on the game thread.
 * Transport owns parsing, initialize/capability negotiation and session lifecycle.
 * A null response means a notification (JSON-RPC notifications never receive replies).
 */
class BSHARNESSTOOLS_API FMCPToolDispatcher
{
public:
	static TFuture<TSharedPtr<FJsonObject>> Dispatch(
		FMCPToolRegistry& Registry, const TSharedRef<FJsonObject>& Request, double TimeoutSeconds = 30.0);
};
