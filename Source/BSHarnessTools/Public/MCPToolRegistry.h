#pragma once

#include "Async/Future.h"
#include "Containers/Ticker.h"
#include "Misc/Crc.h"
#include "MCPToolTypes.h"

struct FMCPPendingToolCall;

struct BSHARNESSTOOLS_API FMCPToolBinding
{
	FMCPToolDefinition Definition;
	FMCPAsyncToolHandler Handler;
};

/**
 * Registry operations and calls belong to the game thread (including destruction).
 * Handlers start on that thread; async completion is thread safe and accepted once.
 * Marshal transport requests to the game thread before calling this registry.
 */
class BSHARNESSTOOLS_API FMCPToolRegistry
{
public:
	FMCPToolRegistry();
	~FMCPToolRegistry();
	FMCPToolRegistry(const FMCPToolRegistry&) = delete;
	FMCPToolRegistry& operator=(const FMCPToolRegistry&) = delete;

	bool RegisterSyncTool(const FMCPToolDefinition& Definition, FMCPSyncToolHandler Handler, FString& OutError);
	bool RegisterAsyncTool(const FMCPToolDefinition& Definition, FMCPAsyncToolHandler Handler, FString& OutError);
	bool UnregisterTool(const FString& Name);
	TArray<FMCPToolDefinition> ListTools() const;
	/** Replace a managed group atomically; unrelated registrations and pending calls survive. */
	bool ReplaceTools(const TArray<FString>& NamesToRemove, const TArray<FMCPToolBinding>& Replacements, FString& OutError);

	/** Sync handlers resolve before return. Never Wait/Get an unfinished future on the game thread. */
	TFuture<FMCPToolResult> CallTool(const FString& Name, const FMCPToolArguments& Arguments, double TimeoutSeconds = 30.0);
	void Shutdown();

private:
	struct FRegisteredTool
	{
		FMCPToolDefinition Definition;
		FMCPAsyncToolHandler Handler;
	};
	struct FToolNameKeyFuncs : BaseKeyFuncs<TPair<FString, FRegisteredTool>, FString>
	{
		static KeyInitType GetSetKey(ElementInitType Element) { return Element.Key; }
		static bool Matches(KeyInitType A, KeyInitType B) { return A.Equals(B, ESearchCase::CaseSensitive); }
		static uint32 GetKeyHash(KeyInitType Key) { return FCrc::StrCrc32(*Key); }
	};

	bool Tick(float DeltaSeconds);
	// Exact MCP tool names are case sensitive. Mutate through registration functions only.
	TMap<FString, FRegisteredTool, FDefaultSetAllocator, FToolNameKeyFuncs> TOOL_HANDLERS;
	TArray<TSharedRef<FMCPPendingToolCall, ESPMode::ThreadSafe>> PendingCalls;
	FTSTicker::FDelegateHandle TickerHandle;
	bool bShuttingDown = false;
};
