#include "AgentSubagentRunner.h"
#include "AgentToolBridge.h"
#include "AgentToolWorkflow.h"
#include "AgentWorkbenchJson.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformTime.h"
#include "Serialization/JsonSerializer.h"

namespace
{
constexpr int32 SubagentMaxInlineResultChars = 16000;

FString SerializeToolResult(const FMCPToolResult& Result)
{
    FString Text;
    FJsonSerializer::Serialize(Result.ToJson(), TJsonWriterFactory<>::Create(&Text));
    if (Text.Len() > SubagentMaxInlineResultChars)
    {
        const int32 OriginalLength = Text.Len();
        TSharedRef<FJsonObject> Truncated = MakeShared<FJsonObject>();
        Truncated->SetBoolField(TEXT("truncated"), true);
        Truncated->SetNumberField(TEXT("original_chars"), OriginalLength);
        Truncated->SetStringField(TEXT("preview"), Text.Left(SubagentMaxInlineResultChars));
        Text.Empty();
        FJsonSerializer::Serialize(Truncated, TJsonWriterFactory<>::Create(&Text));
    }
    return Text;
}
}

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

    // Single-level delegation: the subagent cannot call the task tool.
    Runner->Tools = FAgentToolBridge::ListAvailable(*Runner->Snapshot);
    Runner->Tools.RemoveAll([](const FMCPToolDefinition& Definition)
    {
        return FAgentToolBridge::IsSubagentToolName(Definition.Name);
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
        Finish(FMCPToolResult::Failure(Result.Error.IsEmpty()
            ? TEXT("Subagent model request failed.") : Result.Error));
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
    ToolTurn.Text = SerializeToolResult(Result);
    Conversation.Add(MoveTemp(ToolTurn));
    ++NextToolCallIndex;
    ExecuteNextTool();
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
