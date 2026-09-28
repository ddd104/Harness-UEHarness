#include "AgentWorkbenchHistory.h"
#include "AgentWorkbenchSession.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentHistoryRoundTripTest,
    "AgentWorkbench.History.RoundTripAndCorruption",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentHistoryRoundTripTest::RunTest(const FString& Parameters)
{
    const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"),
        TEXT("AgentWorkbenchHistory"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
    FAgentSession Original;
    Original.Title = TEXT("持久化测试");
    Original.DraftText = TEXT("未发送草稿");
    Original.ModelOptions.Model = TEXT("saved-model");
    Original.ModelOptions.MaxOutputTokens = 321;
    Original.Runner.State = EAgentRunState::Running;
    TSharedPtr<FAgentCandidate> Candidate = MakeShared<FAgentCandidate>();
    Candidate->DisplayName = TEXT("LostBlueprint");
    Candidate->ObjectPath = TEXT("/Game/AgentWorkbenchTest/LostBlueprint.LostBlueprint");
    Candidate->PackageName = TEXT("/Game/AgentWorkbenchTest/LostBlueprint");
    Candidate->AssetClassPath = TEXT("/Script/Engine.Blueprint");
    Candidate->bIncluded = false;
    Original.Candidates.Add(Candidate);
    FAgentRunInputSnapshot Run;
    Run.SessionId = Original.SessionId;
    Run.RunId = FGuid::NewGuid();
    Run.UserInput = TEXT("Previous input");
    Run.ModelOptions = Original.ModelOptions;
    FAgentCandidate Frozen = *Candidate;
    Frozen.bIncluded = true;
    Run.IncludedAssets.Add(Frozen);
    Original.RunHistory.Add(Run);
    TSharedPtr<FAgentMessage> Message = MakeShared<FAgentMessage>();
    Message->Text = Run.UserInput;
    Message->RunId = Run.RunId;
    Message->IncludedAssets.Add(Frozen);
    Original.Messages.Add(Message);
    TestTrue(TEXT("Session JSON saved"), FAgentWorkbenchHistory::Save(Original, Directory));
    const FString BadFile = FPaths::Combine(Directory, FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".json"));
    TestTrue(TEXT("Corrupt neighbor written"), FFileHelper::SaveStringToFile(TEXT("{broken"), *BadFile));
    IAssetRegistry::GetChecked().WaitForCompletion();
    TArray<TSharedPtr<FAgentSession>> Loaded;
    TArray<FString> Errors;
    FAgentWorkbenchHistory::LoadAll(Directory, Loaded, Errors);
    TestEqual(TEXT("One valid Session survives corrupt neighbor"), Loaded.Num(), 1);
    TestEqual(TEXT("Corrupt file is reported"), Errors.Num(), 1);
    if (Loaded.Num() == 1)
    {
        const FAgentSession& Restored = *Loaded[0];
        TestEqual(TEXT("Draft restored"), Restored.DraftText, Original.DraftText);
        TestEqual(TEXT("Model restored"), Restored.ModelOptions.Model, Original.ModelOptions.Model);
        TestEqual(TEXT("Token setting restored"), Restored.ModelOptions.MaxOutputTokens, 321);
        TestEqual(TEXT("Candidate count restored"), Restored.Candidates.Num(), 1);
        if (Restored.Candidates.Num() == 1)
        {
            TestFalse(TEXT("Candidate checkbox restored"), Restored.Candidates[0]->bIncluded);
            TestEqual(TEXT("Stale path retained"), Restored.Candidates[0]->ObjectPath, Candidate->ObjectPath);
            TestEqual(TEXT("Stale path marked missing after registry scan"),
                Restored.Candidates[0]->ValidationStatus, EAgentCandidateValidation::Missing);
        }
        TestEqual(TEXT("Unfinished Run restored as interrupted"), Restored.Runner.State, EAgentRunState::Interrupted);
        TestEqual(TEXT("Previous message restored"), Restored.Messages.Num(), 1);
        TestEqual(TEXT("Run input snapshot restored"), Restored.RunHistory.Num(), 1);
        if (Restored.RunHistory.Num() == 1)
        {
            TestEqual(TEXT("Historical asset path retained"), Restored.RunHistory[0].IncludedAssets[0].ObjectPath, Frozen.ObjectPath);
            TestEqual(TEXT("Historical model retained"), Restored.RunHistory[0].ModelOptions.Model, FString(TEXT("saved-model")));
        }
    }
    IFileManager::Get().Delete(*FPaths::Combine(Directory, Original.SessionId.ToString(EGuidFormats::Digits) + TEXT(".json")));
    IFileManager::Get().Delete(*BadFile);
    IFileManager::Get().DeleteDirectory(*Directory);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentHistoryForkTest,
    "AgentWorkbench.History.ForkValueIsolation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentHistoryForkTest::RunTest(const FString& Parameters)
{
    const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"),
        TEXT("AgentWorkbenchFork"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
    TSharedRef<FAgentSessionManager> Manager = MakeShared<FAgentSessionManager>(Directory);
    TSharedRef<FAgentSession> A = Manager->CreateSession();
    TSharedPtr<FAgentCandidate> Candidate = MakeShared<FAgentCandidate>();
    Candidate->DisplayName = TEXT("Blueprint");
    Candidate->ObjectPath = TEXT("/Game/Test/Blueprint.Blueprint");
    A->Candidates.Add(Candidate);
    FAgentRunInputSnapshot PreviousRun;
    PreviousRun.SessionId = A->SessionId;
    PreviousRun.RunId = FGuid::NewGuid();
    PreviousRun.UserInput = TEXT("Earlier request");
    PreviousRun.IncludedAssets.Add(*Candidate);
    A->RunHistory.Add(PreviousRun);
    A->Touch();
    TSharedRef<FAgentSession> B = Manager->ForkSession(A, false);
    TestNotEqual(TEXT("Fork gets new Session ID"), A->SessionId, B->SessionId);
    TestEqual(TEXT("Fork copies candidate"), B->Candidates.Num(), 1);
    TestEqual(TEXT("Fork copies historical Run input"), B->RunHistory.Num(), 1);
    B->Candidates[0]->bIncluded = false;
    B->Candidates[0]->ObjectPath = TEXT("/Game/Test/Other.Other");
    B->Touch();
    TestTrue(TEXT("Original checkbox unchanged"), A->Candidates[0]->bIncluded);
    TestEqual(TEXT("Original path unchanged"), A->Candidates[0]->ObjectPath, FString(TEXT("/Game/Test/Blueprint.Blueprint")));
    B->Candidates.Empty();
    B->Touch();
    TestEqual(TEXT("Removing fork reference leaves original"), A->Candidates.Num(), 1);
    TestEqual(TEXT("Removing current candidate keeps fork historical Run"), B->RunHistory[0].IncludedAssets[0].ObjectPath,
        FString(TEXT("/Game/Test/Blueprint.Blueprint")));
    Manager->Shutdown();
    TArray<TSharedPtr<FAgentSession>> Loaded;
    TArray<FString> Errors;
    FAgentWorkbenchHistory::LoadAll(Directory, Loaded, Errors, false);
    const TSharedPtr<FAgentSession>* RestoredFork = Loaded.FindByPredicate([&B](const TSharedPtr<FAgentSession>& Item)
        { return Item && Item->SessionId == B->SessionId; });
    TestNotNull(TEXT("Fork reloads from its own file"), RestoredFork);
    if (RestoredFork)
    {
        TestEqual(TEXT("Fork reload keeps inherited historical Run"), (*RestoredFork)->RunHistory.Num(), 1);
        TestEqual(TEXT("Fork reload keeps historical source Session ID"), (*RestoredFork)->RunHistory[0].SessionId, A->SessionId);
    }
    IFileManager::Get().Delete(*FPaths::Combine(Directory, A->SessionId.ToString(EGuidFormats::Digits) + TEXT(".json")));
    IFileManager::Get().Delete(*FPaths::Combine(Directory, B->SessionId.ToString(EGuidFormats::Digits) + TEXT(".json")));
    IFileManager::Get().DeleteDirectory(*Directory);
    return true;
}

#endif
