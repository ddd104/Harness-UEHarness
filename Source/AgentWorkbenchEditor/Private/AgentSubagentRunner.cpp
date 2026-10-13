#include "AgentSubagentRunner.h"
#include "AgentContextCompactor.h"
#include "AgentToolBridge.h"
#include "AgentToolWorkflow.h"
#include "AgentWorkbenchJson.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformTime.h"
#include "Serialization/JsonSerializer.h"

void FAgentSubagentRunner::Run(const TSharedRef<const FAgentRunInputSnapshot>& ParentSnapshot,
    const TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe>& CancellationToken,
    const FString& Prompt, const FAgentSubagentConfig& Config,
    TFunction<void(FMCPToolResult)> Completion)
{
    check(IsInGameThread());
    const TSharedRef<FAgentSubagentRunner> Runner = MakeShared<FAgentSubagentRunner>();

    TSharedPtr<FAgentRunInputSnapshot> MutableSnapshot = MakeShared<FAgentRunInputSnapshot>(*ParentSnapshot);
    // The subagent carries its own conversation; the parent transcript must not leak into it.
    MutableSnapshot->Conversation.Reset();
    if (Config.MaxOutputTokens > 0) { MutableSnapshot->ModelOptions.MaxOutputTokens = Config.MaxOutputTokens; }
    Runner->Snapshot = MutableSnapshot;

    Runner->CancellationToken = CancellationToken;
    Runner->Config = Config;
    Runner->Completion = MoveTemp(Completion);
    Runner->DeadlineSeconds = Config.DeadlineSeconds;
    Runner->ToolWorkflow = MakeShared<FAgentToolWorkflow>();
    Runner->Prompt = Prompt;

    // Single-level delegation: the subagent cannot call the task, compact or memory tools.
    Runner->Tools = FAgentToolBridge::ListAvailable(*Runner->Snapshot);
    Runner->Tools.RemoveAll([](const FMCPToolDefinition& Definition)
    {
        return FAgentToolBridge::IsSubagentToolName(Definition.Name)
            || FAgentToolBridge::IsCompactName(Definition.Name)
            || FAgentToolBridge::IsMemoryRememberName(Definition.Name)
            || FAgentToolBridge::IsMemoryForgetName(Definition.Name);
    });

    FAgentConversationTurn PromptTurn;
    PromptTurn.Role = EAgentMessageRole::User;
    PromptTurn.Text = Prompt;
    Runner->Conversation.Add(MoveTemp(PromptTurn));

    Runner->Begin();
}

void FAgentSubagentRunner::Begin()
{
    SelfKeepAlive = AsShared();
    DeadlineTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
        TEXT("AgentSubagentDeadline"), 0.05f,
        [Weak = TWeakPtr<FAgentSubagentRunner>(AsShared())](float)
        {
            const TSharedPtr<FAgentSubagentRunner> Self = Weak.Pin();
            if (!Self || Self->bFinished) { return false; }
            FString Error;
            if (Self->CheckCancelledOrDeadline(Error))
            {
                if (Self->ActiveRequest) { Self->ActiveRequest->CancelRequest(); Self->ActiveRequest.Reset(); }
                Self->Finish(FMCPToolResult::Failure(Error));
                return false;
            }
            return true;
        });
    UE_LOG(LogTemp, Log, TEXT("[Subagent] started (max %d turns)."), Config.MaxTurns);
    StartTurn();
}

bool FAgentSubagentRunner::CheckCancelledOrDeadline(FString& OutError)
{
    if (CancellationToken.IsValid() && CancellationToken->load())
    {
        OutError = TEXT("Subagent was cancelled.");
        return true;
    }
    if (DeadlineSeconds > 0.0 && FPlatformTime::Seconds() >= DeadlineSeconds)
    {
        OutError = TEXT("Subagent exceeded its total timeout.");
        return true;
    }
    return false;
}

void FAgentSubagentRunner::StartTurn()
{
    if (bFinished) { return; }
    FString Error;
    if (CheckCancelledOrDeadline(Error)) { Finish(FMCPToolResult::Failure(Error)); return; }
    if (Turns >= Config.MaxTurns)
    {
        Finish(FMCPToolResult::Failure(FString::Printf(
            TEXT("Subagent stopped after %d turns without a final answer."), Config.MaxTurns)));
        return;
    }
    ++Turns;
    if (FAgentContextCompactor::Prepare(Conversation, Snapshot->CompactConfig))
    {
        BeginCompactHistory();
        return;
    }
    StartTurnCore();
}

void FAgentSubagentRunner::StartTurnCore()
{
    if (bFinished) { return; }
    RequestId = FGuid::NewGuid();
    UE_LOG(LogTemp, Log, TEXT("[Subagent] turn %d/%d"), Turns, Config.MaxTurns);
    const TSharedPtr<FAgentSubagentRunner> Self = AsShared();
    FAgentModelClient::RunTurn(*Snapshot, Conversation, Tools, RequestId, DeadlineSeconds,
        CancellationToken, ActiveRequest,
        [Weak = TWeakPtr<FAgentSubagentRunner>(Self)](FAgentModelTurnResult Result)
        {
            if (const TSharedPtr<FAgentSubagentRunner> Strong = Weak.Pin())
            { Strong->OnTurnComplete(MoveTemp(Result)); }
        });
}

void FAgentSubagentRunner::OnTurnComplete(FAgentModelTurnResult Result)
{
    if (bFinished) { return; }
    ActiveRequest.Reset();
    FString Error;
    if (CheckCancelledOrDeadline(Error)) { Finish(FMCPToolResult::Failure(Error)); return; }
    if (!Result.bTransportSucceeded || !Result.bParsed)
    {
        const FString Message = Result.Error.IsEmpty()
            ? TEXT("Subagent model request failed.") : Result.Error;
        if ((FAgentContextCompactor::IsContextTooLong(Result.Error)
                || FAgentContextCompactor::IsContextTooLong(Result.ResponseText))
            && ReactiveRetries < Snapshot->CompactConfig.MaxReactiveRetries)
        {
            ++ReactiveRetries;
            BeginReactiveCompact();
            return;
        }
        Finish(FMCPToolResult::Failure(Message));
        return;
    }
    if (Result.ToolCalls.IsEmpty())
    {
        const FString Final = Result.ReplyText.TrimStartAndEnd();
        if (Final.IsEmpty())
        {
            Finish(FMCPToolResult::Failure(TEXT("Subagent returned no summary.")));
            return;
        }
        UE_LOG(LogTemp, Log, TEXT("[Subagent] done after %d turn(s)."), Turns);
        Finish(FMCPToolResult::Success(Final));
        return;
    }

    PendingToolCalls = Result.ToolCalls;
    FAgentConversationTurn AssistantTurn;
    AssistantTurn.Role = EAgentMessageRole::Assistant;
    AssistantTurn.Text = Result.ReplyText;
    AssistantTurn.ToolCalls = MoveTemp(Result.ToolCalls);
    Conversation.Add(MoveTemp(AssistantTurn));
    NextToolCallIndex = 0;
    ExecuteNextTool();
}

void FAgentSubagentRunner::ExecuteNextTool()
{
    if (bFinished) { return; }
    FString Error;
    if (CheckCancelledOrDeadline(Error)) { Finish(FMCPToolResult::Failure(Error)); return; }
    if (NextToolCallIndex >= PendingToolCalls.Num())
    {
        PendingToolCalls.Reset();
        StartTurn();
        return;
    }
    const FAgentToolCall Call = PendingToolCalls[NextToolCallIndex];
    const double Remaining = DeadlineSeconds > 0.0
        ? DeadlineSeconds - FPlatformTime::Seconds()
        : static_cast<double>(Snapshot->ToolTimeoutSeconds);
    const double Timeout = FMath::Min<double>(Snapshot->ToolTimeoutSeconds, Remaining);
    if (Timeout <= 0.0)
    {
        Finish(FMCPToolResult::Failure(TEXT("Subagent exceeded its total timeout.")));
        return;
    }
    UE_LOG(LogTemp, Log, TEXT("[Subagent] tool: %s"), *Call.Name);
    const FAgentToolInvocation Invocation{Snapshot.ToSharedRef(),
        CancellationToken.ToSharedRef(), RequestId, Call, DeadlineSeconds, /*bPreapproved*/ true};
    const TSharedPtr<FAgentSubagentRunner> Self = AsShared();
    ToolWorkflow->Execute(Invocation, Timeout,
        [Weak = TWeakPtr<FAgentSubagentRunner>(Self), CallId = Call.Id, ToolName = Call.Name]
        (FMCPToolResult Result)
        {
            if (const TSharedPtr<FAgentSubagentRunner> Strong = Weak.Pin())
            { Strong->OnToolResult(CallId, ToolName, MoveTemp(Result)); }
        });
}

void FAgentSubagentRunner::OnToolResult(const FString& ToolCallId, const FString& ToolName, FMCPToolResult Result)
{
    if (bFinished) { return; }
    if (NextToolCallIndex >= PendingToolCalls.Num()
        || PendingToolCalls[NextToolCallIndex].Id != ToolCallId) { return; }
    FAgentConversationTurn ToolTurn;
    ToolTurn.Role = EAgentMessageRole::Tool;
    ToolTurn.ToolCallId = ToolCallId;
    ToolTurn.ToolName = ToolName;
    FString ResultText;
    FJsonSerializer::Serialize(Result.ToJson(), TJsonWriterFactory<>::Create(&ResultText));
    ToolTurn.Text = FAgentContextCompactor::FitInlineToolResult(
        ToolCallId, ResultText, Snapshot->CompactConfig);
    Conversation.Add(MoveTemp(ToolTurn));
    ++NextToolCallIndex;
    ExecuteNextTool();
}

void FAgentSubagentRunner::BeginCompactHistory()
{
    if (bFinished) { return; }
    const FString Transcript = FAgentContextCompactor::WriteTranscript(Conversation);
    UE_LOG(LogTemp, Log, TEXT("[Subagent][auto compact] transcript: %s"), *Transcript);
    bCompacting = true;
    const TArray<FAgentConversationTurn> ToSummarize = Conversation;
    const FString ActivePrompt = Prompt;
    const TSharedPtr<FAgentSubagentRunner> Self = AsShared();
    FAgentContextCompactor::RequestSummary(*Snapshot, ToSummarize, Snapshot->CompactConfig,
        DeadlineSeconds, CancellationToken, ActiveRequest,
        [Weak = TWeakPtr<FAgentSubagentRunner>(Self), Transcript, ActivePrompt](bool bOk, const FString& Summary)
        {
            if (const TSharedPtr<FAgentSubagentRunner> Strong = Weak.Pin())
            { Strong->OnCompactHistorySummary(bOk, Summary, Transcript, ActivePrompt); }
        });
}

void FAgentSubagentRunner::OnCompactHistorySummary(bool bOk, const FString& Summary,
    const FString& Transcript, const FString& ActivePrompt)
{
    if (bFinished || !bCompacting) { return; }
    bCompacting = false;
    ActiveRequest.Reset();
    FString Error;
    if (CheckCancelledOrDeadline(Error)) { Finish(FMCPToolResult::Failure(Error)); return; }
    if (!bOk) { Finish(FMCPToolResult::Failure(TEXT("Subagent context compaction failed: ") + Summary)); return; }
    TArray<FAgentConversationTurn> Compacted;
    Compacted.Add(FAgentContextCompactor::SummaryMessage(
        TEXT("Compacted"), ActivePrompt, Summary, Transcript));
    Conversation = MoveTemp(Compacted);
    StartTurnCore();
}

void FAgentSubagentRunner::BeginReactiveCompact()
{
    if (bFinished) { return; }
    const int32 TailStart = FAgentContextCompactor::ReactiveTailStart(
        Conversation, Snapshot->CompactConfig.KeepRecentMessages);
    TArray<FAgentConversationTurn> OldHistory;
    for (int32 Index = 0; Index < TailStart; ++Index) { OldHistory.Add(Conversation[Index]); }
    const FString Transcript = FAgentContextCompactor::WriteTranscript(Conversation);
    bCompacting = true;
    const FString ActivePrompt = Prompt;
    const TSharedPtr<FAgentSubagentRunner> Self = AsShared();
    FAgentContextCompactor::RequestSummary(*Snapshot, OldHistory, Snapshot->CompactConfig,
        DeadlineSeconds, CancellationToken, ActiveRequest,
        [Weak = TWeakPtr<FAgentSubagentRunner>(Self), TailStart, Transcript, ActivePrompt](bool bOk, const FString& Summary)
        {
            if (const TSharedPtr<FAgentSubagentRunner> Strong = Weak.Pin())
            { Strong->OnReactiveCompactSummary(bOk, Summary, TailStart, Transcript, ActivePrompt); }
        });
}

void FAgentSubagentRunner::OnReactiveCompactSummary(bool bOk, const FString& Summary,
    int32 TailStart, const FString& Transcript, const FString& ActivePrompt)
{
    if (bFinished || !bCompacting) { return; }
    bCompacting = false;
    ActiveRequest.Reset();
    FString Error;
    if (CheckCancelledOrDeadline(Error)) { Finish(FMCPToolResult::Failure(Error)); return; }
    if (!bOk) { Finish(FMCPToolResult::Failure(TEXT("Subagent reactive compaction failed: ") + Summary)); return; }
    Conversation = FAgentContextCompactor::BuildReactiveConversation(
        Conversation, TailStart, ActivePrompt, Summary, Transcript);
    StartTurnCore();
}

void FAgentSubagentRunner::Finish(FMCPToolResult Result)
{
    check(IsInGameThread());
    if (bFinished) { return; }
    bFinished = true;
    if (DeadlineTickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(DeadlineTickerHandle);
        DeadlineTickerHandle.Reset();
    }
    if (ActiveRequest) { ActiveRequest->CancelRequest(); ActiveRequest.Reset(); }
    TFunction<void(FMCPToolResult)> CompletionLocal = MoveTemp(Completion);
    CompletionLocal(MoveTemp(Result));
    SelfKeepAlive.Reset();
}
