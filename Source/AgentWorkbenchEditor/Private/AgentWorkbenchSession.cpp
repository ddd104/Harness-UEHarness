#include "AgentWorkbenchSession.h"
#include "AgentAssetContextService.h"
#include "AgentWorkbenchWidgets.h"
#include "AgentWorkbenchHistory.h"
#include "AgentWorkbenchSettings.h"
#include "AgentModelClient.h"
#include "AgentToolBridge.h"
#include "AgentToolWorkflow.h"
#include "BSHarnessHookRegistry.h"
#include "Framework/Application/SlateApplication.h"
#include "Containers/Ticker.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Interfaces/IHttpRequest.h"
#include "Serialization/JsonSerializer.h"
#include "Widgets/SWindow.h"

namespace
{
void ClearRunTimeout(FAgentRunner& Runner)
{
    if (Runner.RunTimeoutHandle.IsValid())
    {
        FTSTicker::RemoveTicker(Runner.RunTimeoutHandle);
        Runner.RunTimeoutHandle.Reset();
    }
}

bool StartModelRequest(const TSharedRef<FAgentSession>& Session, FString& OutError, bool& bTimedOut)
{
    check(IsInGameThread());
    bTimedOut = false;
    if (FPlatformTime::Seconds() >= Session->Runner.RunDeadlineSeconds)
    {
        bTimedOut = true;
        OutError = TEXT("Run exceeded its total timeout.");
        return false;
    }
    Session->Runner.CurrentToolCallId.Empty();
    Session->Runner.CurrentRequestId = FGuid::NewGuid();
    Session->Runner.Phase = EAgentRunPhase::RequestingModel;
    return FAgentModelClient::Start(Session, Session->Runner.CurrentRequestId, OutError);
}

void NotifyRunExited(const FAgentSession& Session, const FGuid& RunId,
    EBSHookRunExitReason Reason, const FString& Detail = FString())
{
    FBSHookRunExitContext Context;
    Context.SessionId = Session.SessionId;
    Context.RunId = RunId;
    Context.Reason = Reason;
    Context.Detail = Detail;
    FBSHarnessHookRegistry::Get().EmitRunExited(Context);
}
}

void FAgentSession::Touch()
{
    UpdatedAt = FDateTime::UtcNow();
    if (OnChanged) { OnChanged(*this); }
    OnUiChanged.Broadcast();
}

void FAgentSession::AddEvent(EAgentEventType Type, const FGuid& RunId, const FString& Summary, const FString& Detail)
{
    TSharedPtr<FAgentEvent> Event = MakeShared<FAgentEvent>();
    Event->Type = Type;
    Event->RunId = RunId;
    Event->Sequence = NextEventSequence++;
    Event->Summary = Summary;
    Event->Detail = Detail;
    Events.Add(Event);
    UpdatedAt = FDateTime::UtcNow();
}

void FAgentRunner::Cancel()
{
    if (State != EAgentRunState::Running) { return; }
    State = EAgentRunState::Cancelled;
    if (CancellationToken) { CancellationToken->store(true); }
    Phase = EAgentRunPhase::None;
    CurrentRequestId.Invalidate();
    CurrentToolCallId.Empty();
    PendingToolCalls.Empty();
    ClearRunTimeout(*this);
    if (ActiveRequest) { ActiveRequest->CancelRequest(); ActiveRequest.Reset(); }
}

bool FAgentSession::BeginRun(FString& OutError)
{
    check(IsInGameThread());
    if (bPromptHookActive)
    {
        OutError = TEXT("A prompt submission hook is already running for this session.");
        return false;
    }
    if (Runner.State == EAgentRunState::Running)
    {
        OutError = TEXT("当前会话已有运行中的任务。");
        return false;
    }
    const FGuid RunId = FGuid::NewGuid();
    FAgentRunInputSnapshot Snapshot;
    if (!FAgentAssetContextService::BuildSnapshot(*this, RunId,
        [](const FSoftObjectPath& Path) { return FAgentAssetContextService::LookupAsset(Path); },
        Snapshot, OutError)) { return false; }
    const UAgentWorkbenchSettings* Settings = GetDefault<UAgentWorkbenchSettings>();
    Snapshot.ProviderType = Settings->ProviderType;
    Snapshot.Provider = FAgentModelClient::ProviderName(Snapshot.ProviderType);
    Snapshot.ApprovalMode = ApprovalMode;
    Snapshot.RequestTimeoutSeconds = FMath::Max(1, Settings->RequestTimeoutSeconds);
    Snapshot.ToolTimeoutSeconds = FMath::Max(1, Settings->ToolTimeoutSeconds);
    Snapshot.RunTimeoutSeconds = FMath::Max(1, Settings->RunTimeoutSeconds);
    Snapshot.MaxToolSteps = FMath::Max(1, Settings->MaxToolSteps);
    FAgentToolBridge::FreezeAvailableTools(Snapshot);
    Snapshot.bToolListFrozen = true;
    TSet<FGuid> CompletedRuns;
    for (const TSharedPtr<FAgentEvent>& Event : Events)
    {
        if (Event && Event->Type == EAgentEventType::RunCompleted)
        { CompletedRuns.Add(Event->RunId); }
    }
    for (const TSharedPtr<FAgentMessage>& Message : Messages)
    {
        if (!Message || Message->Role == EAgentMessageRole::Error
            || (Message->Role == EAgentMessageRole::Assistant && Message->Text.StartsWith(TEXT("[Mock]")))
            || (Message->Role != EAgentMessageRole::User && !CompletedRuns.Contains(Message->RunId)))
        { continue; }
        FAgentConversationTurn Turn;
        Turn.Role = Message->Role;
        Turn.Text = Message->Text;
        Turn.IncludedAssets = Message->IncludedAssets;
        Turn.ToolCalls = Message->ToolCalls;
        Turn.ToolCallId = Message->ToolCallId;
        Turn.ToolName = Message->ToolName;
        Snapshot.Conversation.Add(MoveTemp(Turn));
    }
    FAgentConversationTurn Current;
    Current.Text = Snapshot.UserInput;
    Current.IncludedAssets = Snapshot.IncludedAssets;
    Snapshot.Conversation.Add(MoveTemp(Current));

    FBSHookPromptContext PromptContext;
    PromptContext.SessionId = Snapshot.SessionId;
    PromptContext.RunId = Snapshot.RunId;
    PromptContext.UserPrompt = Snapshot.UserInput;
    PromptContext.ModelId = Snapshot.ModelOptions.Model;
    for (const FAgentCandidate& Asset : Snapshot.IncludedAssets)
    {
        PromptContext.CandidatePaths.Add(Asset.ObjectPath);
    }
    const FBSHookPromptContext OriginalPrompt = PromptContext;
    bPromptHookActive = true;
    const bool bAccepted = FBSHarnessHookRegistry::Get().EmitPromptSubmitting(PromptContext, OutError);
    bPromptHookActive = false;
    if (!bAccepted)
    {
        if (OutError.IsEmpty()) { OutError = TEXT("Agent extension rejected the prompt."); }
        return false;
    }
    if (PromptContext.SessionId != OriginalPrompt.SessionId
        || PromptContext.RunId != OriginalPrompt.RunId
        || PromptContext.UserPrompt != OriginalPrompt.UserPrompt
        || PromptContext.ModelId != OriginalPrompt.ModelId
        || PromptContext.CandidatePaths != OriginalPrompt.CandidatePaths)
    {
        OutError = TEXT("Agent extension changed immutable prompt information.");
        return false;
    }
    constexpr int32 MaxExtensionContextChars = 16000;
    if (PromptContext.AdditionalModelContext.Len() > MaxExtensionContextChars)
    {
        OutError = TEXT("Agent extension context exceeds the 16000 character limit.");
        return false;
    }
    if (!PromptContext.AdditionalModelContext.TrimStartAndEnd().IsEmpty())
    {
        // Keep the submitted message intact; only the frozen model conversation gets context.
        Snapshot.Conversation.Last().Text += TEXT("\n\nAgent extension context (data):\n");
        Snapshot.Conversation.Last().Text += PromptContext.AdditionalModelContext;
    }
    Runner.CurrentRunId = RunId;
    Runner.LastSnapshot = MakeShared<FAgentRunInputSnapshot>(MoveTemp(Snapshot));
    const FAgentRunInputSnapshot& Frozen = *Runner.LastSnapshot;
    Runner.Conversation = Frozen.Conversation;
    Runner.PendingToolCalls.Empty();
    Runner.NextToolCallIndex = 0;
    Runner.ToolSteps = 0;
    Runner.CurrentRequestId.Invalidate();
    Runner.CurrentToolCallId.Empty();
    Runner.RunDeadlineSeconds = FPlatformTime::Seconds() + Frozen.RunTimeoutSeconds;
    Runner.CancellationToken = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
    if (!Runner.ToolWorkflow) { Runner.ToolWorkflow = MakeShared<FAgentToolWorkflow>(); }
    FAgentRunInputSnapshot Historical = Frozen;
    Historical.AllowedToolNames.Reset();
    Historical.AllowedToolHandlerIds.Reset();
    RunHistory.Add(MoveTemp(Historical));
    Runner.State = EAgentRunState::Running;
    Runner.Phase = EAgentRunPhase::None;
    TSharedPtr<FAgentMessage> User = MakeShared<FAgentMessage>();
    User->Role = EAgentMessageRole::User;
    User->Text = Frozen.UserInput;
    User->RunId = RunId;
    User->IncludedAssets = Frozen.IncludedAssets;
    Messages.Add(User);
    if (!bTitleManuallySet && Title == TEXT("新对话")) { Title = Frozen.UserInput.Left(32); }
    DraftText.Empty();
    AddEvent(EAgentEventType::RunStarted, RunId, TEXT("任务开始"));
    // A validated send has now started a Run, so this Session becomes history.
    // Keep this submission single-owner if a post-submit handler calls back into the Session.
    bPromptHookActive = true;
    FBSHarnessHookRegistry::Get().EmitPromptSubmitted(PromptContext);
    Touch();
    bPromptHookActive = false;
    return true;
}

bool FAgentSession::Send(FString& OutError)
{
    if (!FAgentModelClient::Validate(*GetDefault<UAgentWorkbenchSettings>(), ModelOptions.Model, OutError)) { return false; }
    if (!BeginRun(OutError)) { return false; }
    const FGuid RunId = Runner.CurrentRunId;
    // PromptSubmitted handlers may synchronously end the Run.
    if (Runner.State != EAgentRunState::Running) { return true; }
    TWeakPtr<FAgentSession> WeakSession = AsShared();
    Runner.RunTimeoutHandle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda([WeakSession, RunId](float)
        {
            if (const TSharedPtr<FAgentSession> Current = WeakSession.Pin();
                Current && Current->Runner.State == EAgentRunState::Running
                && Current->Runner.CurrentRunId == RunId)
            {
                Current->Runner.RunTimeoutHandle.Reset();
                Current->FailRun(RunId, TEXT("Run exceeded its total timeout."), true);
            }
            return false;
        }), static_cast<float>(Runner.LastSnapshot->RunTimeoutSeconds));
    bool bTimedOut = false;
    if (!StartModelRequest(AsShared(), OutError, bTimedOut))
    {
        FailRun(Runner.CurrentRunId, OutError, bTimedOut);
        return true;
    }
    return true;
}

void FAgentSession::CompleteRun(const FGuid& RunId, const FString& ReplyText)
{
    check(IsInGameThread());
    if (Runner.State != EAgentRunState::Running || Runner.CurrentRunId != RunId) { return; }
    Runner.ActiveRequest.Reset();
    if (Runner.CancellationToken) { Runner.CancellationToken->store(true); }
    Runner.Phase = EAgentRunPhase::None;
    Runner.CurrentRequestId.Invalidate();
    ClearRunTimeout(Runner);
    TSharedPtr<FAgentMessage> Reply = MakeShared<FAgentMessage>();
    Reply->Role = EAgentMessageRole::Assistant;
    Reply->Text = ReplyText;
    Reply->RunId = RunId;
    Messages.Add(Reply);
    Runner.State = EAgentRunState::Completed;
    AddEvent(EAgentEventType::RunCompleted, RunId, TEXT("任务完成"));
    Touch();
    NotifyRunExited(*this, RunId, EBSHookRunExitReason::Completed);
}

void FAgentSession::FailRun(const FGuid& RunId, const FString& Error, bool bTimedOut)
{
    check(IsInGameThread());
    if (Runner.State != EAgentRunState::Running || Runner.CurrentRunId != RunId) { return; }
    Runner.State = EAgentRunState::Failed;
    if (Runner.CancellationToken) { Runner.CancellationToken->store(true); }
    Runner.Phase = EAgentRunPhase::None;
    Runner.CurrentRequestId.Invalidate();
    Runner.CurrentToolCallId.Empty();
    Runner.PendingToolCalls.Empty();
    ClearRunTimeout(Runner);
    if (Runner.ActiveRequest) { Runner.ActiveRequest->CancelRequest(); Runner.ActiveRequest.Reset(); }
    TSharedPtr<FAgentMessage> Failure = MakeShared<FAgentMessage>();
    Failure->Role = EAgentMessageRole::Error;
    Failure->RunId = RunId;
    Failure->Text = Error;
    Messages.Add(Failure);
    AddEvent(EAgentEventType::RunFailed, RunId, TEXT("任务失败"), Error);
    Touch();
    NotifyRunExited(*this, RunId,
        bTimedOut ? EBSHookRunExitReason::TimedOut : EBSHookRunExitReason::Failed, Error);
}

void FAgentSession::OnModelResponse(const FGuid& RunId, const FGuid& RequestId,
    const FString& ReplyText, const TArray<FAgentToolCall>& ToolCalls,
    const FString& ResponseText)
{
    check(IsInGameThread());
    if (Runner.State != EAgentRunState::Running || Runner.Phase != EAgentRunPhase::RequestingModel
        || Runner.CurrentRunId != RunId || Runner.CurrentRequestId != RequestId) { return; }
    Runner.ActiveRequest.Reset();
    if (FPlatformTime::Seconds() >= Runner.RunDeadlineSeconds)
    { FailRun(RunId, TEXT("Run exceeded its total timeout."), true); return; }
    AddEvent(EAgentEventType::ModelRequestCompleted, RunId,
        ToolCalls.IsEmpty() ? TEXT("模型输出 · 回复")
            : FString::Printf(TEXT("模型输出 · 提出 %d 个工具调用"), ToolCalls.Num()),
        ResponseText);
    Touch();
    if (Runner.State != EAgentRunState::Running
        || Runner.Phase != EAgentRunPhase::RequestingModel
        || Runner.CurrentRunId != RunId
        || Runner.CurrentRequestId != RequestId) { return; }
    if (ToolCalls.IsEmpty())
    {
        if (ReplyText.TrimStartAndEnd().IsEmpty())
        { FailRun(RunId, TEXT("Model returned neither a reply nor a tool call.")); return; }
        CompleteRun(RunId, ReplyText);
        return;
    }
    if (ToolCalls.Num() > Runner.LastSnapshot->MaxToolSteps - Runner.ToolSteps)
    { FailRun(RunId, TEXT("Model requested more tool calls than this Run permits.")); return; }
    TSet<FString> SeenCallIds;
    for (const FAgentToolCall& Call : ToolCalls)
    {
        if (Call.Id.IsEmpty() || Call.Name.IsEmpty() || Call.ArgumentsJson.Len() > 65536
            || SeenCallIds.Contains(Call.Id))
        { FailRun(RunId, TEXT("Model returned an invalid or duplicate tool call.")); return; }
        SeenCallIds.Add(Call.Id);
    }
    FAgentConversationTurn AssistantTurn;
    AssistantTurn.Role = EAgentMessageRole::Assistant;
    AssistantTurn.Text = ReplyText;
    AssistantTurn.ToolCalls = ToolCalls;
    Runner.Conversation.Add(AssistantTurn);
    TSharedPtr<FAgentMessage> Proposal = MakeShared<FAgentMessage>();
    Proposal->Role = EAgentMessageRole::Assistant;
    Proposal->RunId = RunId;
    Proposal->Text = ReplyText;
    Proposal->ToolCalls = ToolCalls;
    Messages.Add(Proposal);
    Runner.Phase = EAgentRunPhase::ProcessingTools;
    Runner.PendingToolCalls = ToolCalls;
    Runner.NextToolCallIndex = 0;
    Touch();
    ExecuteNextTool(RunId);
}

void FAgentSession::ExecuteNextTool(const FGuid& RunId)
{
    check(IsInGameThread());
    if (Runner.State != EAgentRunState::Running || Runner.Phase != EAgentRunPhase::ProcessingTools
        || Runner.CurrentRunId != RunId) { return; }
    if (FPlatformTime::Seconds() >= Runner.RunDeadlineSeconds)
    { FailRun(RunId, TEXT("Run exceeded its total timeout."), true); return; }
    if (Runner.NextToolCallIndex >= Runner.PendingToolCalls.Num())
    {
        Runner.PendingToolCalls.Empty();
        FString Error;
        bool bTimedOut = false;
        if (!StartModelRequest(AsShared(), Error, bTimedOut)) { FailRun(RunId, Error, bTimedOut); }
        return;
    }
    const FAgentToolCall Call = Runner.PendingToolCalls[Runner.NextToolCallIndex];
    Runner.CurrentToolCallId = Call.Id;
    ++Runner.ToolSteps;
    AddEvent(EAgentEventType::ToolCallStarted, RunId,
        FString::Printf(TEXT("调用工具：%s"), *Call.Name), Call.ArgumentsJson);
    Touch();
    const double Remaining = Runner.RunDeadlineSeconds - FPlatformTime::Seconds();
    const double Timeout = FMath::Min<double>(Runner.LastSnapshot->ToolTimeoutSeconds, Remaining);
    if (Timeout <= 0.0) { FailRun(RunId, TEXT("Run exceeded its total timeout."), true); return; }
    const FAgentToolInvocation Invocation{Runner.LastSnapshot.ToSharedRef(),
        Runner.CancellationToken.ToSharedRef(), Runner.CurrentRequestId, Call,
        Runner.RunDeadlineSeconds};
    TWeakPtr<FAgentSession> WeakSession = AsShared();
    Runner.ToolWorkflow->Execute(Invocation, Timeout,
        [WeakSession, RunId, CallId = Call.Id, ToolName = Call.Name](FMCPToolResult Result)
        {
            const TSharedPtr<FAgentSession> Current = WeakSession.Pin();
            if (!Current) { return; }
            FString ResultText;
            FJsonSerializer::Serialize(Result.ToJson(), TJsonWriterFactory<>::Create(&ResultText));
            constexpr int32 MaxToolResultChars = 16000;
            if (ResultText.Len() > MaxToolResultChars)
            {
                const int32 OriginalLength = ResultText.Len();
                TSharedRef<FJsonObject> Truncated = MakeShared<FJsonObject>();
                Truncated->SetBoolField(TEXT("truncated"), true);
                Truncated->SetNumberField(TEXT("original_chars"), OriginalLength);
                Truncated->SetStringField(TEXT("preview"), ResultText.Left(MaxToolResultChars));
                ResultText.Empty();
                FJsonSerializer::Serialize(Truncated, TJsonWriterFactory<>::Create(&ResultText));
            }
            Current->OnToolResult(RunId, CallId, ToolName, ResultText);
        });
}

void FAgentSession::OnToolResult(const FGuid& RunId, const FString& ToolCallId,
    const FString& ToolName, const FString& ResultText)
{
    check(IsInGameThread());
    if (Runner.State != EAgentRunState::Running || Runner.Phase != EAgentRunPhase::ProcessingTools
        || Runner.CurrentRunId != RunId || Runner.CurrentToolCallId != ToolCallId
        || !Runner.PendingToolCalls.IsValidIndex(Runner.NextToolCallIndex)
        || Runner.PendingToolCalls[Runner.NextToolCallIndex].Id != ToolCallId) { return; }
    FAgentConversationTurn ToolTurn;
    ToolTurn.Role = EAgentMessageRole::Tool;
    ToolTurn.ToolCallId = ToolCallId;
    ToolTurn.ToolName = ToolName;
    ToolTurn.Text = ResultText;
    Runner.Conversation.Add(ToolTurn);
    TSharedPtr<FAgentMessage> ResultMessage = MakeShared<FAgentMessage>();
    ResultMessage->Role = EAgentMessageRole::Tool;
    ResultMessage->RunId = RunId;
    ResultMessage->ToolCallId = ToolCallId;
    ResultMessage->ToolName = ToolName;
    ResultMessage->Text = ResultText;
    Messages.Add(ResultMessage);
    AddEvent(EAgentEventType::ToolCallCompleted, RunId,
        FString::Printf(TEXT("工具返回：%s"), *ToolName), ResultText);
    Runner.CurrentToolCallId.Empty();
    ++Runner.NextToolCallIndex;
    Touch();
    ExecuteNextTool(RunId);
}

void FAgentSession::CancelRun()
{
    check(IsInGameThread());
    if (Runner.State == EAgentRunState::Running)
    {
        const FGuid RunId = Runner.CurrentRunId;
        Runner.Cancel();
        AddEvent(EAgentEventType::RunCancelled, RunId, TEXT("Run cancelled"));
        Touch();
        NotifyRunExited(*this, RunId, EBSHookRunExitReason::Cancelled);
    }
}

FAgentSessionManager::FAgentSessionManager(const FString& InStorageDirectory)
    : StorageDirectory(InStorageDirectory.IsEmpty() ? FAgentWorkbenchHistory::DefaultDirectory() : InStorageDirectory)
{
    TArray<FString> Errors;
    TArray<TSharedPtr<FAgentSession>> LoadedSessions;
    FAgentWorkbenchHistory::LoadAll(StorageDirectory, LoadedSessions, Errors);
    for (const TSharedPtr<FAgentSession>& Session : LoadedSessions)
    {
        // Older builds saved empty windows. Keep their files untouched, but do not restore
        // them as conversations because no task was ever started.
        if (Session && Session->HasStartedRun())
        {
            AttachPersistence(Session.ToSharedRef());
            Sessions.Add(Session);
        }
    }
    for (const FString& Error : Errors)
    { UE_LOG(LogTemp, Warning, TEXT("AgentWorkbench history: %s"), *Error); }
}

void FAgentSessionManager::AttachPersistence(const TSharedRef<FAgentSession>& Session)
{
    const FString Directory = StorageDirectory;
    Session->OnChanged = [Directory](const FAgentSession& Changed)
    {
        if (!Changed.HasStartedRun()) { return; }
        if (!FAgentWorkbenchHistory::Save(Changed, Directory))
        { UE_LOG(LogTemp, Warning, TEXT("AgentWorkbench: could not save session %s"), *Changed.SessionId.ToString()); }
    };
}

TSharedRef<FAgentSession> FAgentSessionManager::CreateSession()
{
    check(IsInGameThread());
    TSharedRef<FAgentSession> Session = MakeShared<FAgentSession>();
    Session->ModelOptions.Model = GetDefault<UAgentWorkbenchSettings>()->DefaultModel;
    AttachPersistence(Session);
    Sessions.Add(Session);
    OpenSession(Session);
    return Session;
}

TSharedRef<FAgentSession> FAgentSessionManager::ForkSession(const TSharedRef<FAgentSession>& Source, bool bOpenWindow)
{
    check(IsInGameThread());
    TSharedRef<FAgentSession> Fork = MakeShared<FAgentSession>();
    Fork->Title = Source->Title + TEXT(" (分叉)");
    Fork->bTitleManuallySet = true;
    Fork->ModelOptions = Source->ModelOptions;
    Fork->ApprovalMode = Source->ApprovalMode;
    Fork->DraftText = Source->DraftText;
    for (const TSharedPtr<FAgentCandidate>& Candidate : Source->Candidates)
    { if (Candidate) { Fork->Candidates.Add(MakeShared<FAgentCandidate>(*Candidate)); } }
    for (const TSharedPtr<FAgentMessage>& Message : Source->Messages)
    { if (Message) { Fork->Messages.Add(MakeShared<FAgentMessage>(*Message)); } }
    for (const TSharedPtr<FAgentEvent>& Event : Source->Events)
    { if (Event) { Fork->Events.Add(MakeShared<FAgentEvent>(*Event)); } }
    Fork->NextEventSequence = Source->NextEventSequence;
    Fork->RunHistory = Source->RunHistory;
    // Historical Run IDs remain as an audit trail; the fork starts without a live Run.
    Fork->Runner.State = EAgentRunState::Idle;
    AttachPersistence(Fork);
    Sessions.Add(Fork);
    Fork->Touch();
    if (bOpenWindow) { OpenSession(Fork); }
    NotifySessionsChanged();
    return Fork;
}

void FAgentSessionManager::OpenSession(const TSharedRef<FAgentSession>& Session)
{
    check(IsInGameThread());
    if (TSharedPtr<SWindow> Existing = FindWindowForSession(Session->SessionId))
    {
        Existing->BringToFront();
        return;
    }
    TSharedRef<SWindow> Window = SNew(SWindow)
        .Title(FText::FromString(FString::Printf(TEXT("Agent Workbench · %s"), *Session->Title)))
        .ClientSize(FVector2D(1400, 900))
        .MinWidth(1100)
        .MinHeight(700)
        .SizingRule(ESizingRule::UserSized)
        .SupportsMaximize(true)
        .SupportsMinimize(true);
    Window->SetContent(SNew(SAgentChatWindow).Session(Session).Manager(AsShared()));
    TWeakPtr<FAgentSessionManager> WeakManager = AsShared();
    Window->SetRequestDestroyWindowOverride(FRequestDestroyWindowOverride::CreateLambda([WeakManager](const TSharedRef<SWindow>& ClosingWindow)
    {
        if (TSharedPtr<FAgentSessionManager> Manager = WeakManager.Pin())
        {
            for (const auto& Pair : Manager->Windows)
            {
                if (Pair.Value.Pin() != ClosingWindow) { continue; }
                const TSharedPtr<FAgentSession>* Current = Manager->Sessions.FindByPredicate([&Pair](const TSharedPtr<FAgentSession>& Item)
                    { return Item && Item->SessionId == Pair.Key; });
                if (Current && (*Current)->Runner.State == EAgentRunState::Running)
                {
                    if (FMessageDialog::Open(EAppMsgType::YesNo,
                        FText::FromString(TEXT("当前模型请求仍在进行。停止任务并关闭窗口？"))) != EAppReturnType::Yes) { return; }
                    (*Current)->CancelRun();
                }
                break;
            }
        }
        FSlateApplication::Get().RequestDestroyWindow(ClosingWindow);
    }));
    Window->SetOnWindowClosed(FOnWindowClosed::CreateLambda([WeakManager](const TSharedRef<SWindow>& ClosedWindow)
    {
        if (TSharedPtr<FAgentSessionManager> Manager = WeakManager.Pin())
        {
            for (auto It = Manager->Windows.CreateIterator(); It; ++It)
            {
                if (TSharedPtr<SWindow> Mapped = It.Value().Pin())
                {
                    if (Mapped.Get() != &ClosedWindow.Get()) { continue; }
                    const FGuid ClosedSessionId = It.Key();
                    It.RemoveCurrent();
                    for (const TSharedPtr<FAgentSession>& Item : Manager->Sessions)
                    {
                        if (!Item || Item->SessionId != ClosedSessionId) { continue; }
                        if (Item->HasStartedRun()) { Item->Touch(); }
                        else
                        {
                            const TSharedPtr<FAgentSession> UnsentSession = Item;
                            UnsentSession->OnChanged = nullptr;
                            Manager->Sessions.Remove(UnsentSession);
                        }
                        break;
                    }
                    break;
                }
            }
        }
    }));
    Windows.Add(Session->SessionId, Window);
    FSlateApplication::Get().AddWindow(Window);
}

TSharedPtr<SWindow> FAgentSessionManager::FindWindowForSession(const FGuid& SessionId) const
{
    if (const TWeakPtr<SWindow>* Existing = Windows.Find(SessionId)) { return Existing->Pin(); }
    return nullptr;
}

bool FAgentSessionManager::SwitchSessionInWindow(const FGuid& CurrentSessionId, const TSharedRef<FAgentSession>& Target)
{
    check(IsInGameThread());
    if (!Sessions.Contains(Target)) { return false; }
    TSharedPtr<SWindow> Host = FindWindowForSession(CurrentSessionId);
    if (!Host) { return false; }
    if (CurrentSessionId == Target->SessionId) { Host->BringToFront(); return true; }
    const TSharedPtr<FAgentSession>* Source = Sessions.FindByPredicate([&CurrentSessionId](const TSharedPtr<FAgentSession>& Item)
        { return Item && Item->SessionId == CurrentSessionId; });
    if (!Source) { return false; }
    const TSharedPtr<FAgentSession> SourceSession = *Source;
    TSharedPtr<SWindow> Other = FindWindowForSession(Target->SessionId);
    // A displayed Session has one writable view. Swap views if both Sessions are open.
    if (Other && Other != Host)
    {
        Other->SetContent(SNew(SAgentChatWindow).Session(SourceSession).Manager(AsShared()));
        Other->SetTitle(FText::FromString(FString::Printf(TEXT("Agent Workbench · %s"), *SourceSession->Title)));
        Windows.Add(CurrentSessionId, Other);
    }
    else
    {
        Windows.Remove(CurrentSessionId);
        if (!SourceSession->HasStartedRun())
        {
            SourceSession->OnChanged = nullptr;
            Sessions.Remove(SourceSession);
        }
    }
    TSharedRef<SAgentChatWindow> HostContent = SNew(SAgentChatWindow).Session(Target).Manager(AsShared());
    Host->SetContent(HostContent);
    Host->SetTitle(FText::FromString(FString::Printf(TEXT("Agent Workbench · %s"), *Target->Title)));
    Windows.Add(Target->SessionId, Host);
    Host->BringToFront();
    HostContent->FocusHistory();
    return true;
}

void FAgentSessionManager::RequestSwitchSession(const FGuid& CurrentSessionId, const TSharedRef<FAgentSession>& Target)
{
    TWeakPtr<FAgentSessionManager> WeakManager = AsShared();
    FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakManager, CurrentSessionId, TargetId = Target->SessionId](float)
    {
        if (TSharedPtr<FAgentSessionManager> Manager = WeakManager.Pin())
        {
            const TSharedPtr<FAgentSession>* Found = Manager->Sessions.FindByPredicate([&TargetId](const TSharedPtr<FAgentSession>& Item)
                { return Item && Item->SessionId == TargetId; });
            if (Found) { Manager->SwitchSessionInWindow(CurrentSessionId, Found->ToSharedRef()); }
        }
        return false;
    }));
}

bool FAgentSessionManager::DeleteSession(const TSharedRef<FAgentSession>& Target)
{
    check(IsInGameThread());
    if (!Sessions.Contains(Target) || !Target->HasStartedRun()
        || Target->Runner.State == EAgentRunState::Running) { return false; }
    const FString Path = FPaths::Combine(StorageDirectory, Target->SessionId.ToString(EGuidFormats::Digits) + TEXT(".json"));
    if (IFileManager::Get().FileExists(*Path) && !IFileManager::Get().Delete(*Path)) { return false; }
    if (TSharedPtr<SWindow> Window = FindWindowForSession(Target->SessionId))
    {
        Windows.Remove(Target->SessionId);
        const TSharedPtr<FAgentSession>* Fallback = Sessions.FindByPredicate([this, &Target](const TSharedPtr<FAgentSession>& Item)
            { return Item && Item != Target && !FindWindowForSession(Item->SessionId); });
        if (Fallback)
        {
            TSharedRef<SAgentChatWindow> FallbackContent = SNew(SAgentChatWindow).Session(*Fallback).Manager(AsShared());
            Window->SetContent(FallbackContent);
            Window->SetTitle(FText::FromString(FString::Printf(TEXT("Agent Workbench · %s"), *(*Fallback)->Title)));
            Windows.Add((*Fallback)->SessionId, Window);
            FallbackContent->FocusHistory();
        }
        else
        {
            Window->SetOnWindowClosed(FOnWindowClosed());
            FSlateApplication::Get().DestroyWindowImmediately(Window.ToSharedRef());
        }
    }
    Target->OnChanged = nullptr;
    Sessions.Remove(Target);
    NotifySessionsChanged();
    return true;
}

bool FAgentSessionManager::RenameSession(const TSharedRef<FAgentSession>& Target, const FString& NewTitle)
{
    check(IsInGameThread());
    const FString Trimmed = NewTitle.TrimStartAndEnd();
    if (!Sessions.Contains(Target) || Trimmed.IsEmpty() || Trimmed.Len() > 128
        || Trimmed.Contains(TEXT("\n")) || Trimmed.Contains(TEXT("\r"))) { return false; }
    if (Target->Title == Trimmed && Target->bTitleManuallySet) { return true; }
    Target->Title = Trimmed;
    Target->bTitleManuallySet = true;
    Target->Touch();
    UpdateWindowTitle(Target);
    NotifySessionsChanged();
    return true;
}

void FAgentSessionManager::RequestDeleteSession(const TSharedRef<FAgentSession>& Target)
{
    TWeakPtr<FAgentSessionManager> WeakManager = AsShared();
    FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakManager, TargetId = Target->SessionId](float)
    {
        if (TSharedPtr<FAgentSessionManager> Manager = WeakManager.Pin())
        {
            const TSharedPtr<FAgentSession>* Found = Manager->Sessions.FindByPredicate([&TargetId](const TSharedPtr<FAgentSession>& Item)
                { return Item && Item->SessionId == TargetId; });
            if (Found && !Manager->DeleteSession(Found->ToSharedRef()))
            { FMessageDialog::Open(EAppMsgType::Ok, FText::FromString(TEXT("删除会话失败；请检查历史文件是否可写或任务是否正在运行。"))); }
        }
        return false;
    }));
}

void FAgentSessionManager::UpdateWindowTitle(const TSharedRef<FAgentSession>& Session)
{
    if (const TWeakPtr<SWindow>* Existing = Windows.Find(Session->SessionId))
    {
        if (TSharedPtr<SWindow> Window = Existing->Pin())
        {
            Window->SetTitle(FText::FromString(FString::Printf(TEXT("Agent Workbench · %s"), *Session->Title)));
        }
    }
}

void FAgentSessionManager::Shutdown()
{
    check(IsInGameThread());
    for (const TSharedPtr<FAgentSession>& Session : Sessions)
    {
        if (!Session) { continue; }
        // Every prior state change has already been saved. A Running state is read back as
        // Interrupted on startup. Do not create a fresh UDeveloperSettings CDO while
        // editor modules and UObject classes are being torn down.
        Session->OnChanged = nullptr;
        if (Session->Runner.State == EAgentRunState::Running)
        {
            const FGuid RunId = Session->Runner.CurrentRunId;
            Session->Runner.Cancel();
            NotifyRunExited(*Session, RunId, EBSHookRunExitReason::Closed);
        }
    }
    for (auto& Pair : Windows)
    {
        if (TSharedPtr<SWindow> Window = Pair.Value.Pin())
        {
            Window->SetOnWindowClosed(FOnWindowClosed());
            FSlateApplication::Get().DestroyWindowImmediately(Window.ToSharedRef());
        }
    }
    Windows.Empty();
    Sessions.Empty();
}

int32 FAgentSessionManager::GetOpenWindowCount() const
{
    int32 Count = 0;
    for (const auto& Pair : Windows) { if (Pair.Value.IsValid()) { ++Count; } }
    return Count;
}

TArray<TSharedPtr<FAgentSession>> FAgentSessionManager::GetHistorySessions() const
{
    TArray<TSharedPtr<FAgentSession>> Result;
    for (const TSharedPtr<FAgentSession>& Session : Sessions)
    { if (Session && Session->HasStartedRun()) { Result.Add(Session); } }
    return Result;
}
