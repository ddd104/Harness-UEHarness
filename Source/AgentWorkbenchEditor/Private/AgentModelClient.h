#pragma once

#include "CoreMinimal.h"
#include "AgentWorkbenchSettings.h"
#include "AgentWorkbenchSession.h"
#include "MCPToolTypes.h"
#include <atomic>

class FAgentSession;
class IHttpRequest;

struct FAgentModelTurnResult
{
    bool bTransportSucceeded = false;
    bool bParsed = false;
    int32 HttpStatus = 0;
    FString ReplyText;
    TArray<FAgentToolCall> ToolCalls;
    FString ResponseText;
    FString Error;
};

class FAgentModelClient
{
public:
    static FString ProviderName(EAgentWorkbenchProvider Provider);
    static bool Validate(const UAgentWorkbenchSettings& Settings, const FString& Model, FString& OutError);
    static bool Start(const TSharedRef<FAgentSession>& Session, const FGuid& RequestId, FString& OutError);

    // One model turn decoupled from FAgentSession: builds the request, sends it, parses the
    // reply and tool calls. Completion always runs on the game thread. OutActiveRequest is
    // set to the in-flight request (or reset) so the caller can cancel it.
    static void RunTurn(
        const FAgentRunInputSnapshot& Snapshot,
        const TArray<FAgentConversationTurn>& Conversation,
        const TArray<FMCPToolDefinition>& Tools,
        const FGuid& RequestId,
        double DeadlineSeconds,
        const TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe>& CancellationToken,
        TSharedPtr<IHttpRequest, ESPMode::ThreadSafe>& OutActiveRequest,
        TFunction<void(FAgentModelTurnResult)> Completion);
};
