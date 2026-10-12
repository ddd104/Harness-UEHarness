#pragma once

#include "CoreMinimal.h"
#include "AgentModelClient.h"
#include "MCPToolTypes.h"
#include "Containers/Ticker.h"
#include <atomic>

class FAgentToolWorkflow;

struct FAgentSubagentConfig
{
    int32 MaxTurns = 12;
    // 0 keeps the parent run's model output token limit.
    int32 MaxOutputTokens = 0;
    // Absolute deadline; 0 means unbounded.
    double DeadlineSeconds = 0.0;
};

/**
 * Nested Agent Loop used by the "task" tool. Runs with a fresh conversation, the parent
 * run's frozen tool permissions minus the task tool, and returns only its final text.
 * Inner tool calls inherit the parent run's approval (the task tool was approved once).
 */
class FAgentSubagentRunner : public TSharedFromThis<FAgentSubagentRunner>
{
public:
    static void Run(const TSharedRef<const FAgentRunInputSnapshot>& ParentSnapshot,
        const TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe>& CancellationToken,
        const FString& Prompt, const FAgentSubagentConfig& Config,
        TFunction<void(FMCPToolResult)> Completion);

private:
    void Begin();
    void StartTurn();
    void StartTurnCore();
    void OnTurnComplete(FAgentModelTurnResult Result);
    void ExecuteNextTool();
    void OnToolResult(const FString& ToolCallId, const FString& ToolName, FMCPToolResult Result);
    void BeginCompactHistory();
    void OnCompactHistorySummary(bool bOk, const FString& Summary,
        const FString& Transcript, const FString& ActiveRequest);
    void BeginReactiveCompact();
    void OnReactiveCompactSummary(bool bOk, const FString& Summary,
        int32 TailStart, const FString& Transcript, const FString& ActiveRequest);
    void Finish(FMCPToolResult Result);
    bool CheckCancelledOrDeadline(FString& OutError);

    TSharedPtr<FAgentSubagentRunner> SelfKeepAlive;
    TSharedPtr<const FAgentRunInputSnapshot> Snapshot;
    TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> CancellationToken;
    FAgentSubagentConfig Config;
    TFunction<void(FMCPToolResult)> Completion;
    TArray<FAgentConversationTurn> Conversation;
    TArray<FMCPToolDefinition> Tools;
    TArray<FAgentToolCall> PendingToolCalls;
    int32 NextToolCallIndex = 0;
    int32 Turns = 0;
    double DeadlineSeconds = 0.0;
    FGuid RequestId;
    TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> ActiveRequest;
    TSharedPtr<FAgentToolWorkflow> ToolWorkflow;
    FTSTicker::FDelegateHandle DeadlineTickerHandle;
    FString Prompt;
    bool bCompacting = false;
    int32 ReactiveRetries = 0;
    bool bFinished = false;
};
