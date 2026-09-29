#include "AgentWorkbenchHistory.h"
#include "AgentWorkbenchDisplayFormatter.h"
#include "AgentWorkbenchSession.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"

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
    Run.Provider = TEXT("Google Gemini");
    Run.ProviderType = EAgentWorkbenchProvider::Gemini;
    Run.RequestTimeoutSeconds = 30;
    Run.ToolTimeoutSeconds = 12;
    Run.RunTimeoutSeconds = 150;
    Run.MaxToolSteps = 5;
    Run.bToolListFrozen = true;
    Run.AllowedToolNames.Add(TEXT("assets.search"));
    Run.AllowedToolHandlerIds.Add(TEXT("assets.search"), TEXT("assets.search"));
    FAgentCandidate Frozen = *Candidate;
    Frozen.bIncluded = true;
    Run.IncludedAssets.Add(Frozen);
    FAgentToolCall ToolCall;
    ToolCall.Id = TEXT("call-123");
    ToolCall.Name = TEXT("assets.search");
    ToolCall.ArgumentsJson = TEXT("{\"query\":\"Test\"}");
    ToolCall.ThoughtSignature = TEXT("opaque-provider-signature");
    FAgentConversationTurn AssistantTurn;
    AssistantTurn.Role = EAgentMessageRole::Assistant;
    AssistantTurn.ToolCalls.Add(ToolCall);
    Run.Conversation.Add(AssistantTurn);
    FAgentConversationTurn ToolTurn;
    ToolTurn.Role = EAgentMessageRole::Tool;
    ToolTurn.Text = TEXT("{\"assets\":[]}");
    ToolTurn.ToolCallId = ToolCall.Id;
    ToolTurn.ToolName = ToolCall.Name;
    Run.Conversation.Add(ToolTurn);
    Original.RunHistory.Add(Run);
    TSharedPtr<FAgentMessage> Message = MakeShared<FAgentMessage>();
    Message->Text = Run.UserInput;
    Message->RunId = Run.RunId;
    Message->IncludedAssets.Add(Frozen);
    Original.Messages.Add(Message);
    TSharedPtr<FAgentMessage> AssistantMessage = MakeShared<FAgentMessage>();
    AssistantMessage->Role = EAgentMessageRole::Assistant;
    AssistantMessage->RunId = Run.RunId;
    AssistantMessage->ToolCalls.Add(ToolCall);
    Original.Messages.Add(AssistantMessage);
    TSharedPtr<FAgentMessage> ToolMessage = MakeShared<FAgentMessage>();
    ToolMessage->Role = EAgentMessageRole::Tool;
    ToolMessage->RunId = Run.RunId;
    ToolMessage->Text = ToolTurn.Text;
    ToolMessage->ToolCallId = ToolCall.Id;
    ToolMessage->ToolName = ToolCall.Name;
    Original.Messages.Add(ToolMessage);
    const FString FailureReason = TEXT("模型请求失败：连接超时。");
    TSharedPtr<FAgentMessage> ErrorMessage = MakeShared<FAgentMessage>();
    ErrorMessage->Role = EAgentMessageRole::Error;
    ErrorMessage->RunId = Run.RunId;
    ErrorMessage->Text = FailureReason;
    Original.Messages.Add(ErrorMessage);
    Original.AddEvent(EAgentEventType::ToolCallCompleted, Run.RunId, TEXT("assets.search"));
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
        TestEqual(TEXT("Previous messages restored"), Restored.Messages.Num(), 4);
        if (Restored.Messages.Num() == 4)
        {
            TestEqual(TEXT("Assistant tool call count restored"), Restored.Messages[1]->ToolCalls.Num(), 1);
            if (Restored.Messages[1]->ToolCalls.Num() == 1)
            {
                TestEqual(TEXT("Assistant tool call ID restored"), Restored.Messages[1]->ToolCalls[0].Id, ToolCall.Id);
                TestEqual(TEXT("Assistant tool arguments restored"), Restored.Messages[1]->ToolCalls[0].ArgumentsJson, ToolCall.ArgumentsJson);
                TestEqual(TEXT("Provider thought signature restored"), Restored.Messages[1]->ToolCalls[0].ThoughtSignature,
                    ToolCall.ThoughtSignature);
            }
            TestEqual(TEXT("Tool result matches call ID"), Restored.Messages[2]->ToolCallId, ToolCall.Id);
            TestEqual(TEXT("Tool result matches call name"), Restored.Messages[2]->ToolName, ToolCall.Name);
            TestEqual(TEXT("Failure role restored"), Restored.Messages[3]->Role, EAgentMessageRole::Error);
            TestEqual(TEXT("Failure Run ID restored"), Restored.Messages[3]->RunId, Run.RunId);
            TestEqual(TEXT("Failure reason restored"), Restored.Messages[3]->Text, FailureReason);
            const TArray<TSharedPtr<FAgentMessage>> Visible =
                FAgentWorkbenchDisplayFormatter::VisibleConversationMessages(Restored.Messages);
            TestEqual(TEXT("Restored conversation shows question and failure"), Visible.Num(), 2);
            if (Visible.Num() == 2)
            {
                TestTrue(TEXT("Restored question is visible first"), Visible[0] == Restored.Messages[0]);
                TestTrue(TEXT("Restored failure is visible after question"), Visible[1] == Restored.Messages[3]);
            }
        }
        TestEqual(TEXT("Tool event restored"), Restored.Events.Num(), 1);
        if (Restored.Events.Num() == 1)
        { TestEqual(TEXT("Tool event type restored"), Restored.Events[0]->Type, EAgentEventType::ToolCallCompleted); }
        TestEqual(TEXT("Run input snapshot restored"), Restored.RunHistory.Num(), 1);
        if (Restored.RunHistory.Num() == 1)
        {
            TestEqual(TEXT("Historical asset path retained"), Restored.RunHistory[0].IncludedAssets[0].ObjectPath, Frozen.ObjectPath);
            TestEqual(TEXT("Historical model retained"), Restored.RunHistory[0].ModelOptions.Model, FString(TEXT("saved-model")));
            TestEqual(TEXT("Historical provider type retained"), Restored.RunHistory[0].ProviderType, EAgentWorkbenchProvider::Gemini);
            TestEqual(TEXT("Historical request timeout retained"), Restored.RunHistory[0].RequestTimeoutSeconds, 30);
            TestEqual(TEXT("Historical tool timeout retained"), Restored.RunHistory[0].ToolTimeoutSeconds, 12);
            TestEqual(TEXT("Historical run timeout retained"), Restored.RunHistory[0].RunTimeoutSeconds, 150);
            TestEqual(TEXT("Historical tool step limit retained"), Restored.RunHistory[0].MaxToolSteps, 5);
            TestTrue(TEXT("Historical tool list remains frozen"), Restored.RunHistory[0].bToolListFrozen);
            TestTrue(TEXT("Historical allowed tool names are omitted"), Restored.RunHistory[0].AllowedToolNames.IsEmpty());
            TestTrue(TEXT("Historical handler bindings are omitted"), Restored.RunHistory[0].AllowedToolHandlerIds.IsEmpty());
            TestEqual(TEXT("Historical conversation turn count"), Restored.RunHistory[0].Conversation.Num(), 2);
            if (Restored.RunHistory[0].Conversation.Num() == 2)
            {
                TestEqual(TEXT("Historical call count retained"), Restored.RunHistory[0].Conversation[0].ToolCalls.Num(), 1);
                if (Restored.RunHistory[0].Conversation[0].ToolCalls.Num() == 1)
                { TestEqual(TEXT("Historical call signature retained"),
                    Restored.RunHistory[0].Conversation[0].ToolCalls[0].ThoughtSignature, ToolCall.ThoughtSignature); }
                TestEqual(TEXT("Historical result call ID retained"), Restored.RunHistory[0].Conversation[1].ToolCallId, ToolCall.Id);
            }
        }
    }
    IFileManager::Get().Delete(*FPaths::Combine(Directory, Original.SessionId.ToString(EGuidFormats::Digits) + TEXT(".json")));
    IFileManager::Get().Delete(*BadFile);
    IFileManager::Get().DeleteDirectory(*Directory);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentHistoryLegacySchemaTest,
    "AgentWorkbench.History.LegacySchema",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentHistoryLegacySchemaTest::RunTest(const FString& Parameters)
{
    const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"),
        TEXT("AgentWorkbenchLegacyHistory"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
    const FGuid SessionId = FGuid::NewGuid();
    const FGuid RunId = FGuid::NewGuid();
    const FGuid MessageId = FGuid::NewGuid();
    const FString LegacyJson = FString::Printf(
        TEXT("{\"version\":1,\"session_id\":\"%s\",\"title\":\"Old session\",")
        TEXT("\"run_state\":0,\"messages\":[{\"message_id\":\"%s\",\"role\":0,\"text\":\"old text\"}],")
        TEXT("\"events\":[{\"sequence\":1,\"type\":4,\"summary\":\"cancelled\"}],")
        TEXT("\"run_history\":[{\"session_id\":\"%s\",\"run_id\":\"%s\",\"user_input\":\"old text\",")
        TEXT("\"model\":\"old-model\",\"provider\":\"Anthropic\"}]}"),
        *SessionId.ToString(), *MessageId.ToString(), *SessionId.ToString(), *RunId.ToString());
    TestTrue(TEXT("Legacy directory created"), IFileManager::Get().MakeDirectory(*Directory, true));
    const FString Path = FPaths::Combine(Directory, SessionId.ToString(EGuidFormats::Digits) + TEXT(".json"));
    TestTrue(TEXT("Legacy JSON written"), FFileHelper::SaveStringToFile(LegacyJson, *Path));
    TArray<TSharedPtr<FAgentSession>> Loaded;
    TArray<FString> Errors;
    FAgentWorkbenchHistory::LoadAll(Directory, Loaded, Errors, false);
    TestEqual(TEXT("Legacy file loads"), Loaded.Num(), 1);
    TestTrue(TEXT("Legacy file has no errors"), Errors.IsEmpty());
    if (Loaded.Num() == 1)
    {
        TestEqual(TEXT("Legacy message retained"), Loaded[0]->Messages.Num(), 1);
        TestEqual(TEXT("Legacy event retained"), Loaded[0]->Events.Num(), 1);
        if (Loaded[0]->Events.Num() == 1)
        { TestEqual(TEXT("Legacy cancelled event type unchanged"), Loaded[0]->Events[0]->Type, EAgentEventType::RunCancelled); }
        TestEqual(TEXT("Legacy run retained"), Loaded[0]->RunHistory.Num(), 1);
        if (Loaded[0]->RunHistory.Num() == 1)
        {
            TestEqual(TEXT("Legacy provider inferred"), Loaded[0]->RunHistory[0].ProviderType, EAgentWorkbenchProvider::Anthropic);
            TestEqual(TEXT("Legacy request timeout defaulted"), Loaded[0]->RunHistory[0].RequestTimeoutSeconds, 120);
            TestEqual(TEXT("Legacy tool timeout defaulted"), Loaded[0]->RunHistory[0].ToolTimeoutSeconds, 60);
            TestEqual(TEXT("Legacy run timeout defaulted"), Loaded[0]->RunHistory[0].RunTimeoutSeconds, 900);
            TestEqual(TEXT("Legacy tool step limit defaulted"), Loaded[0]->RunHistory[0].MaxToolSteps, 16);
            TestFalse(TEXT("Legacy tool list is unfrozen"), Loaded[0]->RunHistory[0].bToolListFrozen);
            TestTrue(TEXT("Legacy tool list empty"), Loaded[0]->RunHistory[0].AllowedToolNames.IsEmpty());
        }
    }
    IFileManager::Get().Delete(*Path);
    IFileManager::Get().DeleteDirectory(*Directory);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentHistoryToolBindingOmissionTest,
    "AgentWorkbench.History.ToolBindingOmission",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentHistoryToolBindingOmissionTest::RunTest(const FString& Parameters)
{
    const FString Directory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"),
        TEXT("AgentWorkbenchCatalogHistory"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
    FAgentSession Session;
    FAgentRunInputSnapshot Run;
    Run.SessionId = Session.SessionId;
    Run.RunId = FGuid::NewGuid();
    Run.UserInput = TEXT("List blueprint variables");
    Run.ModelOptions.Model = TEXT("test-model");
    Run.bToolListFrozen = true;
    const auto AddBinding = [&Run](const FString& Name, const FString& HandlerId)
    {
        Run.AllowedToolNames.Add(Name);
        Run.AllowedToolHandlerIds.Add(Name, HandlerId);
    };
    AddBinding(TEXT("bsharness.assets.get"), TEXT("assets.get"));
    AddBinding(TEXT("ue_mcp.catalog.search"), TEXT("ue_mcp.meta.search.v1"));
    AddBinding(TEXT("ue_mcp.catalog.describe"), TEXT("ue_mcp.meta.describe.v1"));
    AddBinding(TEXT("ue_mcp.call_tool"), TEXT("ue_mcp.meta.call.v1"));
    AddBinding(TEXT("ue_mcp.custom_native"), TEXT("assets.search"));
    constexpr int32 CatalogSize = 876;
    for (int32 Index = 0; Index < CatalogSize; ++Index)
    {
        const FString Name = FString::Printf(TEXT("ue_mcp.TestTool_%04d"), Index);
        const TCHAR* Prefix = Index == 0 ? TEXT("ue_mcp.top") : TEXT("ue_mcp.toolset");
        const FString Handler = FString::Printf(TEXT("%s.instance_%d.schema_%d"), Prefix, Index, Index);
        AddBinding(Name, Handler);
    }
    Session.RunHistory.Add(Run);
    TestTrue(TEXT("Catalog history saved"), FAgentWorkbenchHistory::Save(Session, Directory));
    TestEqual(TEXT("Save does not mutate the active frozen catalog"),
        Session.RunHistory[0].AllowedToolNames.Num(), CatalogSize + 5);
    TestEqual(TEXT("Save does not mutate the active frozen handlers"),
        Session.RunHistory[0].AllowedToolHandlerIds.Num(), CatalogSize + 5);

    const FString Path = FPaths::Combine(Directory, Session.SessionId.ToString(EGuidFormats::Digits) + TEXT(".json"));
    const auto ReadFile = [&Path](TSharedPtr<FJsonObject>& Out)
    {
        FString Text;
        return FFileHelper::LoadFileToString(Text, *Path)
            && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Out) && Out.IsValid();
    };
    TSharedPtr<FJsonObject> Root;
    if (!TestTrue(TEXT("Compact JSON is valid"), ReadFile(Root)))
    {
        IFileManager::Get().Delete(*Path);
        IFileManager::Get().DeleteDirectory(*Directory);
        return false;
    }
    TSharedPtr<FJsonObject> SavedRun = Root->GetArrayField(TEXT("run_history"))[0]->AsObject();
    if (!TestTrue(TEXT("Saved Run exists"), SavedRun.IsValid()))
    {
        IFileManager::Get().Delete(*Path);
        IFileManager::Get().DeleteDirectory(*Directory);
        return false;
    }
    TestFalse(TEXT("Historical names are not serialized"), SavedRun->HasField(TEXT("allowed_tool_names")));
    TestFalse(TEXT("Historical handler bindings are not serialized"), SavedRun->HasField(TEXT("allowed_tool_handler_ids")));

    // Simulate an older Session file containing the entire catalog, then verify
    // it still loads and becomes compact the next time it is saved.
    TArray<TSharedPtr<FJsonValue>> LegacyNames;
    TSharedRef<FJsonObject> LegacyHandlers = MakeShared<FJsonObject>();
    for (const FString& Name : Run.AllowedToolNames)
    {
        LegacyNames.Add(MakeShared<FJsonValueString>(Name));
    }
    for (const auto& Entry : Run.AllowedToolHandlerIds)
    {
        LegacyHandlers->SetStringField(Entry.Key, Entry.Value);
    }
    SavedRun->SetArrayField(TEXT("allowed_tool_names"), MoveTemp(LegacyNames));
    SavedRun->SetObjectField(TEXT("allowed_tool_handler_ids"), LegacyHandlers);
    FString LegacyText;
    TestTrue(TEXT("Legacy full-catalog JSON serialized"),
        FJsonSerializer::Serialize(Root.ToSharedRef(), TJsonWriterFactory<>::Create(&LegacyText)));
    TestTrue(TEXT("Legacy full-catalog JSON written"), FFileHelper::SaveStringToFile(LegacyText, *Path));
    TArray<TSharedPtr<FAgentSession>> Loaded;
    TArray<FString> Errors;
    FAgentWorkbenchHistory::LoadAll(Directory, Loaded, Errors, false);
    TestTrue(TEXT("Legacy full-catalog file loads without errors"), Errors.IsEmpty());
    TestEqual(TEXT("Legacy full-catalog Session loads"), Loaded.Num(), 1);
    if (Loaded.Num() == 1 && Loaded[0]->RunHistory.Num() == 1)
    {
        TestEqual(TEXT("Legacy catalog bindings remain readable"),
            Loaded[0]->RunHistory[0].AllowedToolHandlerIds.Num(), CatalogSize + 5);
        TestTrue(TEXT("Legacy Session resaved"), FAgentWorkbenchHistory::Save(*Loaded[0], Directory));
        Root.Reset();
        if (TestTrue(TEXT("Resaved JSON is valid"), ReadFile(Root)))
        {
            SavedRun = Root->GetArrayField(TEXT("run_history"))[0]->AsObject();
            TestFalse(TEXT("Legacy catalog names omitted on resave"), SavedRun->HasField(TEXT("allowed_tool_names")));
            TestFalse(TEXT("Legacy catalog handlers omitted on resave"), SavedRun->HasField(TEXT("allowed_tool_handler_ids")));
        }
    }
    IFileManager::Get().Delete(*Path);
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
