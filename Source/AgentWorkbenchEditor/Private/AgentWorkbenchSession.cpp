#include "AgentWorkbenchSession.h"
#include "AgentAssetContextService.h"
#include "AgentWorkbenchWidgets.h"
#include "AgentWorkbenchHistory.h"
#include "AgentWorkbenchSettings.h"
#include "AgentModelClient.h"
#include "Framework/Application/SlateApplication.h"
#include "Containers/Ticker.h"
#include "HAL/FileManager.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Interfaces/IHttpRequest.h"
#include "Widgets/SWindow.h"

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
    if (ActiveRequest) { ActiveRequest->CancelRequest(); ActiveRequest.Reset(); }
}

bool FAgentSession::BeginRun(FString& OutError)
{
    check(IsInGameThread());
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
    Snapshot.Provider = FAgentModelClient::ProviderName(GetDefault<UAgentWorkbenchSettings>()->ProviderType);
    for (const TSharedPtr<FAgentMessage>& Message : Messages)
    {
        if (!Message || (Message->Role != EAgentMessageRole::User && Message->Role != EAgentMessageRole::Assistant)
            || (Message->Role == EAgentMessageRole::Assistant && Message->Text.StartsWith(TEXT("[Mock]")))) { continue; }
        FAgentConversationTurn Turn;
        Turn.Role = Message->Role;
        Turn.Text = Message->Text;
        Turn.IncludedAssets = Message->IncludedAssets;
        Snapshot.Conversation.Add(MoveTemp(Turn));
    }
    FAgentConversationTurn Current;
    Current.Text = Snapshot.UserInput;
    Current.IncludedAssets = Snapshot.IncludedAssets;
    Snapshot.Conversation.Add(MoveTemp(Current));
    Runner.CurrentRunId = RunId;
    Runner.LastSnapshot = MakeShared<FAgentRunInputSnapshot>(MoveTemp(Snapshot));
    const FAgentRunInputSnapshot& Frozen = *Runner.LastSnapshot;
    RunHistory.Add(Frozen);
    Runner.State = EAgentRunState::Running;
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
    Touch();
    return true;
}

bool FAgentSession::Send(FString& OutError)
{
    if (!FAgentModelClient::Validate(*GetDefault<UAgentWorkbenchSettings>(), ModelOptions.Model, OutError)) { return false; }
    if (!BeginRun(OutError)) { return false; }
    if (!FAgentModelClient::Start(AsShared(), OutError))
    {
        FailRun(Runner.CurrentRunId, OutError);
        return true;
    }
    return true;
}

void FAgentSession::CompleteRun(const FGuid& RunId, const FString& ReplyText)
{
    check(IsInGameThread());
    if (Runner.State != EAgentRunState::Running || Runner.CurrentRunId != RunId) { return; }
    Runner.ActiveRequest.Reset();
    TSharedPtr<FAgentMessage> Reply = MakeShared<FAgentMessage>();
    Reply->Role = EAgentMessageRole::Assistant;
    Reply->Text = ReplyText;
    Reply->RunId = RunId;
    Messages.Add(Reply);
    AddEvent(EAgentEventType::ModelRequestCompleted, RunId, TEXT("模型已回复"), Reply->Text);
    Runner.State = EAgentRunState::Completed;
    AddEvent(EAgentEventType::RunCompleted, RunId, TEXT("任务完成"));
    Touch();
}

void FAgentSession::FailRun(const FGuid& RunId, const FString& Error)
{
    check(IsInGameThread());
    if (Runner.State != EAgentRunState::Running || Runner.CurrentRunId != RunId) { return; }
    Runner.ActiveRequest.Reset();
    Runner.State = EAgentRunState::Failed;
    TSharedPtr<FAgentMessage> Failure = MakeShared<FAgentMessage>();
    Failure->Role = EAgentMessageRole::Error;
    Failure->RunId = RunId;
    Failure->Text = Error;
    Messages.Add(Failure);
    AddEvent(EAgentEventType::ModelRequestCompleted, RunId, TEXT("模型请求失败"), Error);
    Touch();
}

void FAgentSession::CancelRun()
{
    if (Runner.State == EAgentRunState::Running)
    {
        Runner.Cancel();
        AddEvent(EAgentEventType::RunCancelled, Runner.CurrentRunId, TEXT("Run cancelled"));
        Touch();
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
    { if (Session && Session->HasStartedRun()) { Session->CancelRun(); Session->Touch(); } }
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
