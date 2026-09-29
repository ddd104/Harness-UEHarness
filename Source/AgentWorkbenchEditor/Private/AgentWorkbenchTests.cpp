#include "AgentWorkbenchSession.h"
#include "AgentWorkbenchHistory.h"
#include "AgentWorkbenchSettings.h"
#include "Misc/AutomationTest.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "ToolMenu.h"
#include "ToolMenus.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentWorkbenchSessionIsolationTest,
    "AgentWorkbench.P1.SessionIsolation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentWorkbenchSessionIsolationTest::RunTest(const FString& Parameters)
{
    FAgentSession A;
    FAgentSession B;
    A.ModelOptions.Model = TEXT("Model A");
    A.ApprovalMode = EAgentApprovalMode::Unrestricted;
    B.ModelOptions.Model = TEXT("Model B");
    A.DraftText = TEXT("Message A");
    B.DraftText = TEXT("Message B");
    TestNotEqual(TEXT("Session IDs are unique"), A.SessionId, B.SessionId);
    FString Error;
    TestTrue(TEXT("A starts a run"), A.BeginRun(Error));
    TestEqual(TEXT("Approval mode is frozen at send"), A.Runner.LastSnapshot->ApprovalMode,
        EAgentApprovalMode::Unrestricted);
    A.ApprovalMode = EAgentApprovalMode::Ask;
    TestEqual(TEXT("Changing next-run approval does not change active run"),
        A.Runner.LastSnapshot->ApprovalMode, EAgentApprovalMode::Unrestricted);
    TestEqual(TEXT("Other session keeps its own approval mode"), B.ApprovalMode, EAgentApprovalMode::Ask);
    A.CompleteRun(A.Runner.CurrentRunId, TEXT("Provider response"));
    TestEqual(TEXT("A has a user and provider reply"), A.Messages.Num(), 2);
    TestEqual(TEXT("B has no messages"), B.Messages.Num(), 0);
    TestEqual(TEXT("B draft remains independent"), B.DraftText, FString(TEXT("Message B")));
    TestEqual(TEXT("B model remains independent"), B.ModelOptions.Model, FString(TEXT("Model B")));
    TestTrue(TEXT("A run has its own ID"), A.Runner.CurrentRunId.IsValid());
    TestFalse(TEXT("B has no run"), B.Runner.CurrentRunId.IsValid());
    TestEqual(TEXT("A run completed"), A.Runner.State, EAgentRunState::Completed);
    TestEqual(TEXT("A records structured events"), A.Events.Num(), 3);
    FAgentSession Cancelled;
    Cancelled.DraftText = TEXT("Stop this request");
    TestTrue(TEXT("Cancellation fixture starts"), Cancelled.BeginRun(Error));
    const FGuid CancelledRunId = Cancelled.Runner.CurrentRunId;
    Cancelled.CancelRun();
    Cancelled.CompleteRun(CancelledRunId, TEXT("Late response"));
    TestEqual(TEXT("Cancelled run ignores late response"), Cancelled.Runner.State, EAgentRunState::Cancelled);
    TestEqual(TEXT("No fabricated late assistant message"), Cancelled.Messages.Num(), 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentWorkbenchShutdownPersistenceTest,
    "AgentWorkbench.History.ShutdownKeepsLastSafeSave",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentWorkbenchShutdownPersistenceTest::RunTest(const FString& Parameters)
{
    const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"),
        TEXT("AgentWorkbenchShutdown"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
    TSharedRef<FAgentSessionManager> Manager = MakeShared<FAgentSessionManager>(Directory);
    TSharedRef<FAgentSession> Session = Manager->CreateSession();
    Session->DraftText = TEXT("Started before shutdown");
    FString Error;
    TestTrue(TEXT("Run starts and is persisted"), Session->BeginRun(Error));
    Manager->Shutdown();
    TestEqual(TEXT("In-memory work is cancelled"), Session->Runner.State,
        EAgentRunState::Cancelled);
    TArray<TSharedPtr<FAgentSession>> Restored;
    TArray<FString> Errors;
    FAgentWorkbenchHistory::LoadAll(Directory, Restored, Errors, false);
    TestEqual(TEXT("Last safe save is readable"), Restored.Num(), 1);
    if (Restored.Num() == 1)
    {
        TestEqual(TEXT("Run resumes as interrupted instead of replaying"),
            Restored[0]->Runner.State, EAgentRunState::Interrupted);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentWorkbenchSettingsTest,
    "AgentWorkbench.P1.SettingsDefaults",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentWorkbenchSettingsTest::RunTest(const FString& Parameters)
{
    const UAgentWorkbenchSettings* Settings = GetDefault<UAgentWorkbenchSettings>();
    TestEqual(TEXT("Project provider defaults to DeepSeek"), Settings->ProviderType, EAgentWorkbenchProvider::DeepSeek);
    TestFalse(TEXT("Default model is a real model"), Settings->DefaultModel.Equals(TEXT("Mock"), ESearchCase::IgnoreCase));
    TestFalse(TEXT("An API key environment variable name is configured"), Settings->ApiKeyEnvironmentVariable.IsEmpty());
    TestEqual(TEXT("Request timeout"), Settings->RequestTimeoutSeconds, 120);
    TestEqual(TEXT("Tool timeout"), Settings->ToolTimeoutSeconds, 60);
    TestEqual(TEXT("Run timeout"), Settings->RunTimeoutSeconds, 900);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentWorkbenchMultiWindowTest,
    "AgentWorkbench.P1.MultiWindow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentWorkbenchMultiWindowTest::RunTest(const FString& Parameters)
{
    if (!FSlateApplication::IsInitialized())
    {
        AddError(TEXT("Slate is required for the multi-window test."));
        return false;
    }
    const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"),
        TEXT("AgentWorkbenchNewWindows"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
    TSharedRef<FAgentSessionManager> Manager = MakeShared<FAgentSessionManager>(Directory);
    TSharedRef<FAgentSession> A = Manager->CreateSession();
    TSharedRef<FAgentSession> B = Manager->CreateSession();
    TSharedRef<FAgentSession> C = Manager->CreateSession();
    TestEqual(TEXT("Three windows are open"), Manager->GetOpenWindowCount(), 3);
    TestNotEqual(TEXT("A and B have distinct IDs"), A->SessionId, B->SessionId);
    TestNotEqual(TEXT("B and C have distinct IDs"), B->SessionId, C->SessionId);
    TestEqual(TEXT("Opening blank windows creates no history rows"), Manager->GetHistorySessions().Num(), 0);
    A->DraftText = TEXT("Unsent draft"); A->Touch();
    TestFalse(TEXT("Unsent draft does not create a history file"), IFileManager::Get().FileExists(
        *FPaths::Combine(Directory, A->SessionId.ToString(EGuidFormats::Digits) + TEXT(".json"))));
    Manager->OpenSession(A);
    TestEqual(TEXT("Opening an existing window activates it"), Manager->GetOpenWindowCount(), 3);
    FSlateApplication::Get().DestroyWindowImmediately(Manager->FindWindowForSession(A->SessionId).ToSharedRef());
    TestEqual(TEXT("Closing an unsent window removes its temporary Session"), Manager->GetSessions().Num(), 2);
    TestEqual(TEXT("Other windows stay open"), Manager->GetOpenWindowCount(), 2);
    Manager->Shutdown();
    TestEqual(TEXT("Shutdown releases window mappings"), Manager->GetOpenWindowCount(), 0);
    TestFalse(TEXT("Shutdown does not save an unsent window"), IFileManager::Get().FileExists(
        *FPaths::Combine(Directory, A->SessionId.ToString(EGuidFormats::Digits) + TEXT(".json"))));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentWorkbenchHistoryOnFirstSendTest,
    "AgentWorkbench.History.OnFirstSend",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentWorkbenchHistoryOnFirstSendTest::RunTest(const FString& Parameters)
{
    const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"),
        TEXT("AgentWorkbenchFirstSend"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
    TSharedRef<FAgentSessionManager> Manager = MakeShared<FAgentSessionManager>(Directory);
    TSharedRef<FAgentSession> Session = Manager->CreateSession();
    const FString File = FPaths::Combine(Directory, Session->SessionId.ToString(EGuidFormats::Digits) + TEXT(".json"));
    FString Error;
    TestFalse(TEXT("Empty send is rejected"), Session->BeginRun(Error));
    TestEqual(TEXT("Rejected send does not enter history"), Manager->GetHistorySessions().Num(), 0);
    Session->DraftText = TEXT("First real task");
    Session->Touch();
    TestFalse(TEXT("Draft alone is not saved as history"), IFileManager::Get().FileExists(*File));
    TSharedPtr<FAgentCandidate> Invalid = MakeShared<FAgentCandidate>();
    Invalid->ObjectPath = TEXT("/Engine/NotAllowed.NotAllowed");
    Session->Candidates.Add(Invalid);
    TestFalse(TEXT("Candidate validation failure does not start a task"), Session->BeginRun(Error));
    TestEqual(TEXT("Failed validation leaves history empty"), Manager->GetHistorySessions().Num(), 0);
    Session->Candidates.Empty();
    TestTrue(TEXT("First valid send starts the task"), Session->BeginRun(Error));
    Session->CompleteRun(Session->Runner.CurrentRunId, TEXT("Provider response"));
    TestEqual(TEXT("Started task appears exactly once in history"), Manager->GetHistorySessions().Num(), 1);
    TestTrue(TEXT("Started task is persisted"), IFileManager::Get().FileExists(*File));
    TestEqual(TEXT("History uses first message as title"), Session->Title, FString(TEXT("First real task")));
    TArray<TSharedPtr<FAgentSession>> Loaded;
    TArray<FString> LoadErrors;
    FAgentWorkbenchHistory::LoadAll(Directory, Loaded, LoadErrors, false);
    TestEqual(TEXT("Exactly one conversation restores"), Loaded.Num(), 1);
    Manager->Shutdown();
    FAgentSession LegacyBlank;
    TestTrue(TEXT("Legacy empty session fixture saved"), FAgentWorkbenchHistory::Save(LegacyBlank, Directory));
    TSharedRef<FAgentSessionManager> Reloaded = MakeShared<FAgentSessionManager>(Directory);
    TestEqual(TEXT("Reload ignores a previously saved empty window"), Reloaded->GetHistorySessions().Num(), 1);
    Reloaded->Shutdown();
    IFileManager::Get().Delete(*File);
    IFileManager::Get().Delete(*FPaths::Combine(Directory,
        LegacyBlank.SessionId.ToString(EGuidFormats::Digits) + TEXT(".json")));
    IFileManager::Get().DeleteDirectory(*Directory);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentWorkbenchToolbarTest,
    "AgentWorkbench.P1.ToolbarRegistration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentWorkbenchToolbarTest::RunTest(const FString& Parameters)
{
    UToolMenu* Menu = UToolMenus::Get()->FindMenu(TEXT("LevelEditor.LevelEditorToolBar.User"));
    TestNotNull(TEXT("Workbench extends the rendered Level Editor user toolbar"), Menu);
    if (Menu)
    {
        FToolMenuSection* Section = Menu->FindSection(TEXT("AgentWorkbench"));
        TestNotNull(TEXT("Workbench toolbar section is registered"), Section);
        if (Section) { TestNotNull(TEXT("New conversation entry is registered"), Section->FindEntry(TEXT("AgentWorkbenchNewConversation"))); }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentWorkbenchHistoryNavigationTest,
    "AgentWorkbench.History.InPlaceNavigationAndDelete",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentWorkbenchHistoryNavigationTest::RunTest(const FString& Parameters)
{
    const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"),
        TEXT("AgentWorkbenchNavigation"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
    TSharedRef<FAgentSessionManager> Manager = MakeShared<FAgentSessionManager>(Directory);
    TSharedRef<FAgentSession> A = Manager->CreateSession();
    TSharedRef<FAgentSession> B = Manager->CreateSession();
    TSharedRef<FAgentSession> C = Manager->CreateSession();
    FString InitialSendError;
    A->DraftText = TEXT("Initial A");
    B->DraftText = TEXT("Initial B");
    C->DraftText = TEXT("Initial C");
    TestTrue(TEXT("A is a historical conversation"), A->BeginRun(InitialSendError));
    A->CompleteRun(A->Runner.CurrentRunId, TEXT("A reply"));
    TestTrue(TEXT("B is a historical conversation"), B->BeginRun(InitialSendError));
    B->CompleteRun(B->Runner.CurrentRunId, TEXT("B reply"));
    TestTrue(TEXT("C is a historical conversation"), C->BeginRun(InitialSendError));
    C->CompleteRun(C->Runner.CurrentRunId, TEXT("C reply"));
    A->DraftText = TEXT("A draft"); A->Touch();
    B->DraftText = TEXT("B draft"); B->Touch();
    TSharedPtr<SWindow> OriginalAWindow = Manager->FindWindowForSession(A->SessionId);
    TSharedPtr<SWindow> OriginalBWindow = Manager->FindWindowForSession(B->SessionId);
    TestTrue(TEXT("Switch into an already open Session"), Manager->SwitchSessionInWindow(A->SessionId, B));
    TestEqual(TEXT("History click keeps physical window count"), Manager->GetOpenWindowCount(), 3);
    TestTrue(TEXT("Target Session is now in clicked window"), Manager->FindWindowForSession(B->SessionId) == OriginalAWindow);
    TestTrue(TEXT("Source Session moves to other window"), Manager->FindWindowForSession(A->SessionId) == OriginalBWindow);
    TestEqual(TEXT("Source draft remains with source Session"), A->DraftText, FString(TEXT("A draft")));
    TestEqual(TEXT("Target draft remains with target Session"), B->DraftText, FString(TEXT("B draft")));
    TestFalse(TEXT("Blank rename is rejected"), Manager->RenameSession(B, TEXT("  ")));
    TestTrue(TEXT("History title can be renamed"), Manager->RenameSession(B, TEXT("  Renamed B  ")));
    TestEqual(TEXT("Renamed Session keeps trimmed title"), B->Title, FString(TEXT("Renamed B")));
    TestEqual(TEXT("Displayed window title follows Session rename"),
        Manager->FindWindowForSession(B->SessionId)->GetTitle().ToString(), FString(TEXT("Agent Workbench · Renamed B")));
    TArray<TSharedPtr<FAgentSession>> SavedSessions;
    TArray<FString> LoadErrors;
    FAgentWorkbenchHistory::LoadAll(Directory, SavedSessions, LoadErrors, false);
    const TSharedPtr<FAgentSession>* SavedB = SavedSessions.FindByPredicate([&B](const TSharedPtr<FAgentSession>& Item)
        { return Item && Item->SessionId == B->SessionId; });
    TestNotNull(TEXT("Renamed Session is in history storage"), SavedB);
    if (SavedB)
    {
        TestEqual(TEXT("Renamed title is persisted"), (*SavedB)->Title, FString(TEXT("Renamed B")));
        TestTrue(TEXT("Manual title flag is persisted"), (*SavedB)->bTitleManuallySet);
    }
    TestTrue(TEXT("Renaming to default label is allowed"), Manager->RenameSession(B, TEXT("新对话")));
    FString SendError;
    TestTrue(TEXT("Send succeeds after manual rename"), B->BeginRun(SendError));
    B->CompleteRun(B->Runner.CurrentRunId, TEXT("B second reply"));
    TestEqual(TEXT("Automatic first-message title does not overwrite manual rename"), B->Title, FString(TEXT("新对话")));
    TSharedRef<FAgentSession> Fork = Manager->ForkSession(C, false);
    TestEqual(TEXT("Context fork does not create another window"), Manager->GetOpenWindowCount(), 3);
    TestTrue(TEXT("Fork replaces current window content"), Manager->SwitchSessionInWindow(B->SessionId, Fork));
    TestTrue(TEXT("Fork uses clicked window"), Manager->FindWindowForSession(Fork->SessionId) == OriginalAWindow);
    TestFalse(TEXT("Previous Session is no longer open"), Manager->FindWindowForSession(B->SessionId).IsValid());
    TestTrue(TEXT("Delete an open history Session"), Manager->DeleteSession(A));
    TestEqual(TEXT("Delete reuses its window for an unopened Session"), Manager->GetOpenWindowCount(), 3);
    TestTrue(TEXT("Fallback Session uses the deleted Session window"), Manager->FindWindowForSession(B->SessionId) == OriginalBWindow);
    TestFalse(TEXT("Deleted Session is absent from history"), Manager->GetSessions().Contains(A));
    TestFalse(TEXT("Deleted Session JSON is removed"), IFileManager::Get().FileExists(
        *FPaths::Combine(Directory, A->SessionId.ToString(EGuidFormats::Digits) + TEXT(".json"))));
    Manager->Shutdown();
    for (const TSharedRef<FAgentSession>& Item : {A, B, C, Fork})
    { IFileManager::Get().Delete(*FPaths::Combine(Directory, Item->SessionId.ToString(EGuidFormats::Digits) + TEXT(".json"))); }
    IFileManager::Get().DeleteDirectory(*Directory);
    return true;
}

#endif
