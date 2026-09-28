#include "MCPToolRegistry.h"

#include "HAL/PlatformTime.h"
#include <atomic>

namespace
{
	TSharedRef<FJsonObject> CloneJson(const TSharedRef<FJsonObject>& Source)
	{
		TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>();
		FJsonObject::Duplicate(TSharedPtr<const FJsonObject>(Source), Copy);
		return Copy.ToSharedRef();
	}

	TFuture<FMCPToolResult> ReadyResult(FMCPToolResult Result)
	{
		TPromise<FMCPToolResult> Promise;
		TFuture<FMCPToolResult> Future = Promise.GetFuture();
		Promise.SetValue(MoveTemp(Result));
		return Future;
	}
}

struct FMCPPendingToolCall
{
	TPromise<FMCPToolResult> Promise;
	double Deadline = 0.0;
	std::atomic<bool> bCompleted{false};

	void Complete(FMCPToolResult Result)
	{
		// SetValue may invoke a continuation inline. Do not hold any registry lock here.
		if (!bCompleted.exchange(true))
		{
			if (FPlatformTime::Seconds() >= Deadline)
			{
				Result = FMCPToolResult::Failure(TEXT("Tool call timed out; underlying work may still be running."));
			}
			Promise.SetValue(MoveTemp(Result));
		}
	}
};

FMCPToolRegistry::FMCPToolRegistry()
{
	check(IsInGameThread());
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FMCPToolRegistry::Tick));
}

FMCPToolRegistry::~FMCPToolRegistry()
{
	Shutdown();
}

bool FMCPToolRegistry::RegisterSyncTool(const FMCPToolDefinition& Definition, FMCPSyncToolHandler Handler, FString& OutError)
{
	if (!Handler)
	{
		OutError = TEXT("Tool handler is empty.");
		return false;
	}
	return RegisterAsyncTool(Definition,
		[Handler = MoveTemp(Handler)](const FMCPToolArguments& Arguments, FMCPToolCompletion Complete)
		{
			Complete(Handler(Arguments));
		}, OutError);
}

bool FMCPToolRegistry::RegisterAsyncTool(const FMCPToolDefinition& Definition, FMCPAsyncToolHandler Handler, FString& OutError)
{
	OutError.Reset();
	if (!IsInGameThread() || bShuttingDown)
	{
		OutError = TEXT("Registration requires an active registry on the game thread.");
		return false;
	}
	FString SchemaType;
	if (Definition.Name.IsEmpty() || Definition.Name.Len() > 128 || !Handler || !Definition.InputSchema.IsValid()
		|| !Definition.InputSchema->TryGetStringField(TEXT("type"), SchemaType) || !SchemaType.Equals(TEXT("object"), ESearchCase::CaseSensitive))
	{
		OutError = TEXT("Tool requires a name (1-128 characters), a handler and an object inputSchema.");
		return false;
	}
	for (TCHAR Character : Definition.Name)
	{
		if (!((Character >= 'a' && Character <= 'z') || (Character >= 'A' && Character <= 'Z')
			|| (Character >= '0' && Character <= '9') || Character == '_' || Character == '-' || Character == '.'))
		{
			OutError = TEXT("Tool names may contain only ASCII letters, digits, underscore, hyphen and dot.");
			return false;
		}
	}
	if (TOOL_HANDLERS.Contains(Definition.Name))
	{
		OutError = FString::Printf(TEXT("Tool already registered: %s"), *Definition.Name);
		return false;
	}
	FRegisteredTool Entry{Definition, MoveTemp(Handler)};
	Entry.Definition.InputSchema = CloneJson(Definition.InputSchema.ToSharedRef());
	TOOL_HANDLERS.Add(Definition.Name, MoveTemp(Entry));
	return true;
}

bool FMCPToolRegistry::UnregisterTool(const FString& Name)
{
	return IsInGameThread() && !bShuttingDown && TOOL_HANDLERS.Remove(Name) > 0;
}

bool FMCPToolRegistry::ReplaceTools(const TArray<FString>& NamesToRemove, const TArray<FMCPToolBinding>& Replacements, FString& OutError)
{
	OutError.Reset();
	if (!IsInGameThread() || bShuttingDown)
	{
		OutError = TEXT("Reload requires an active registry on the game thread.");
		return false;
	}
	// Validate in isolation using the normal registration path, then commit one map swap.
	FMCPToolRegistry Staged;
	Staged.TOOL_HANDLERS = TOOL_HANDLERS;
	for (const FString& Name : NamesToRemove)
	{
		Staged.TOOL_HANDLERS.Remove(Name);
	}
	for (const FMCPToolBinding& Binding : Replacements)
	{
		if (!Staged.RegisterAsyncTool(Binding.Definition, Binding.Handler, OutError))
		{
			return false;
		}
	}
	Swap(TOOL_HANDLERS, Staged.TOOL_HANDLERS);
	return true;
}

TArray<FMCPToolDefinition> FMCPToolRegistry::ListTools() const
{
	check(IsInGameThread());
	TArray<FMCPToolDefinition> Definitions;
	for (const auto& Pair : TOOL_HANDLERS)
	{
		FMCPToolDefinition Copy = Pair.Value.Definition;
		Copy.InputSchema = CloneJson(Copy.InputSchema.ToSharedRef());
		Definitions.Add(MoveTemp(Copy));
	}
	Definitions.Sort([](const FMCPToolDefinition& A, const FMCPToolDefinition& B)
	{
		return A.Name.Compare(B.Name, ESearchCase::CaseSensitive) < 0;
	});
	return Definitions;
}

TFuture<FMCPToolResult> FMCPToolRegistry::CallTool(const FString& Name, const FMCPToolArguments& Arguments, double TimeoutSeconds)
{
	if (!IsInGameThread())
	{
		return ReadyResult(FMCPToolResult::ProtocolError(-32603, TEXT("Dispatch tool calls on the game thread.")));
	}
	if (bShuttingDown)
	{
		return ReadyResult(FMCPToolResult::Failure(TEXT("Tool registry is shut down.")));
	}
	if (!FMath::IsFinite(TimeoutSeconds) || TimeoutSeconds <= 0.0)
	{
		return ReadyResult(FMCPToolResult::ProtocolError(-32602, TEXT("Timeout must be finite and positive.")));
	}
	const FRegisteredTool* Entry = TOOL_HANDLERS.Find(Name);
	if (!Entry)
	{
		return ReadyResult(FMCPToolResult::ProtocolError(-32602, FString::Printf(TEXT("Unknown tool: %s"), *Name)));
	}
	// Copy before executing: a handler/continuation can register or remove tools reentrantly.
	FMCPAsyncToolHandler Handler = Entry->Handler;
	auto Call = MakeShared<FMCPPendingToolCall, ESPMode::ThreadSafe>();
	Call->Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
	TFuture<FMCPToolResult> Future = Call->Promise.GetFuture();
	PendingCalls.Add(Call);
	Handler(CloneJson(Arguments), [Call](FMCPToolResult Result) { Call->Complete(MoveTemp(Result)); });
	return Future;
}

bool FMCPToolRegistry::Tick(float DeltaSeconds)
{
	check(IsInGameThread());
	const double Now = FPlatformTime::Seconds();
	TArray<TSharedRef<FMCPPendingToolCall, ESPMode::ThreadSafe>> Expired;
	for (int32 Index = PendingCalls.Num() - 1; Index >= 0; --Index)
	{
		const auto Call = PendingCalls[Index];
		if (Call->bCompleted.load() || Now >= Call->Deadline)
		{
			PendingCalls.RemoveAtSwap(Index);
			if (!Call->bCompleted.load())
			{
				Expired.Add(Call);
			}
		}
	}
	for (const auto& Call : Expired)
	{
		Call->Complete(FMCPToolResult::Failure(TEXT("Tool call timed out; underlying work may still be running.")));
	}
	return true;
}

void FMCPToolRegistry::Shutdown()
{
	check(IsInGameThread());
	if (bShuttingDown)
	{
		return;
	}
	bShuttingDown = true;
	FTSTicker::RemoveTicker(TickerHandle);
	TOOL_HANDLERS.Empty();
	auto Calls = MoveTemp(PendingCalls);
	for (const auto& Call : Calls)
	{
		Call->Complete(FMCPToolResult::Failure(TEXT("Tool registry shut down before the call completed.")));
	}
}
