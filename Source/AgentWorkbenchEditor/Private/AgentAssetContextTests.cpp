#include "AgentAssetContextService.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Animation/AnimBlueprint.h"
#include "Engine/Texture2D.h"
#include "UObject/ObjectRedirector.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
FAssetData MakeAsset(const TCHAR* Package, const TCHAR* Name, const UClass* Class)
{
    const FString PackageString(Package);
    return FAssetData(FName(Package), FName(*FPackageName::GetLongPackagePath(PackageString)),
        FName(Name), Class->GetClassPathName());
}

FAgentAssetLookupResult Found(const FAssetData& Asset)
{
    FAgentAssetLookupResult Result;
    Result.Status = EAgentCandidateValidation::Valid;
    Result.Data = Asset;
    return Result;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentCandidateSelectionTest,
    "AgentWorkbench.P2.FilterAppendDeduplicate",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentCandidateSelectionTest::RunTest(const FString& Parameters)
{
    FAgentSession Session;
    const FAssetData Blueprint = MakeAsset(TEXT("/Game/AgentP2/RealBlueprint"), TEXT("RealBlueprint"), UBlueprint::StaticClass());
    const FAssetData Texture = MakeAsset(TEXT("/Game/AgentP2/BP_Fake"), TEXT("BP_Fake"), UTexture2D::StaticClass());
    const FAssetData Redirector = MakeAsset(TEXT("/Game/AgentP2/Moved"), TEXT("Moved"), UObjectRedirector::StaticClass());
    const FAssetData EngineBlueprint = MakeAsset(TEXT("/Engine/AgentP2/Outside"), TEXT("Outside"), UBlueprint::StaticClass());
    const FAgentAddCandidatesResult First = FAgentAssetContextService::AddAssets(Session, {Blueprint, Texture, Redirector, EngineBlueprint}, 64);
    TestEqual(TEXT("Blueprint and texture in /Game are added"), First.Added, 2);
    TestEqual(TEXT("Redirector and out-of-root asset are skipped"), First.Skipped, 2);
    TestEqual(TEXT("Redirector is skipped"), First.UnsupportedType, 1);
    TestEqual(TEXT("Out-of-root asset skipped by path"), First.InvalidPath, 1);
    TestEqual(TEXT("Two candidates remain"), Session.Candidates.Num(), 2);
    if (Session.Candidates.Num() != 2) { return false; }
    TestEqual(TEXT("Non-blueprint content asset keeps its real object path"), Session.Candidates[1]->ObjectPath,
        FString(TEXT("/Game/AgentP2/BP_Fake.BP_Fake")));
    Session.Candidates[0]->bIncluded = false;
    const FAgentAddCandidatesResult Again = FAgentAssetContextService::AddAssets(Session, {Blueprint}, 64);
    TestEqual(TEXT("Repeated asset is counted as duplicate"), Again.Duplicates, 1);
    TestFalse(TEXT("Duplicate does not silently re-enable existing candidate"), Session.Candidates[0]->bIncluded);
    const FAssetData Second = MakeAsset(TEXT("/Game/AgentP2/Second"), TEXT("Second"), UBlueprint::StaticClass());
    FAgentAssetContextService::AddAssets(Session, {Second}, 64);
    TestEqual(TEXT("Subsequent add appends"), Session.Candidates.Num(), 3);
    const FAssetData DerivedBlueprint = MakeAsset(TEXT("/Game/AgentP2/Anim"), TEXT("Anim"), UAnimBlueprint::StaticClass());
    const FAgentAddCandidatesResult Derived = FAgentAssetContextService::AddAssets(Session, {DerivedBlueprint}, 64);
    TestEqual(TEXT("Recognized UBlueprint subclass is accepted"), Derived.Added, 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentCandidateLimitTest,
    "AgentWorkbench.P2.LimitAndEmpty",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentCandidateLimitTest::RunTest(const FString& Parameters)
{
    FAgentSession Session;
    const FAgentAddCandidatesResult Empty = FAgentAssetContextService::AddAssets(Session, {}, 2);
    TestEqual(TEXT("Empty selection adds nothing"), Empty.Added, 0);
    TestTrue(TEXT("Empty selection gives actionable feedback"), Empty.Message.Contains(TEXT("主内容浏览器")));
    TestTrue(TEXT("Empty selection reports all counts"), Empty.Message.Contains(TEXT("新增 0，重复 0，跳过 0")));
    const FAssetData A = MakeAsset(TEXT("/Game/AgentP2/A"), TEXT("A"), UBlueprint::StaticClass());
    const FAssetData B = MakeAsset(TEXT("/Game/AgentP2/B"), TEXT("B"), UBlueprint::StaticClass());
    const FAssetData C = MakeAsset(TEXT("/Game/AgentP2/C"), TEXT("C"), UBlueprint::StaticClass());
    const FAgentAddCandidatesResult Limited = FAgentAssetContextService::AddAssets(Session, {A, B, C}, 2);
    TestEqual(TEXT("Two assets added at limit"), Limited.Added, 2);
    TestEqual(TEXT("Overflow is reported"), Limited.OverLimit, 1);
    TestEqual(TEXT("Session has exactly two references"), Session.Candidates.Num(), 2);
    FString Path;
    TestFalse(TEXT("Outside root rejected"), FAgentAssetContextService::NormalizeObjectPath(TEXT("/Engine/AgentP2/C.C"), Path));
    TestFalse(TEXT("Malformed object path rejected"), FAgentAssetContextService::NormalizeObjectPath(TEXT("/Game/AgentP2/Bad"), Path));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentSnapshotTest,
    "AgentWorkbench.P2.SnapshotIsolation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentSnapshotTest::RunTest(const FString& Parameters)
{
    FAgentSession A;
    FAgentSession B;
    const FAssetData Asset = MakeAsset(TEXT("/Game/AgentP2/Enemy"), TEXT("Enemy"), UBlueprint::StaticClass());
    FAgentAssetContextService::AddAssets(A, {Asset}, 64);
    A.Candidates[0]->bIncluded = false; // Legacy saved flag has no checkbox in the path-only UI.
    A.DraftText = TEXT("Inspect Enemy");
    A.ModelOptions.Model = TEXT("Model A");
    A.ModelOptions.MaxOutputTokens = 123;
    B.DraftText = TEXT("Plain chat");
    const FGuid RunId = FGuid::NewGuid();
    FAgentRunInputSnapshot Snapshot;
    FString Error;
    int32 LookupCalls = 0;
    auto Lookup = [&Asset, &LookupCalls](const FSoftObjectPath&) { ++LookupCalls; return Found(Asset); };
    TestTrue(TEXT("A snapshot validates"), FAgentAssetContextService::BuildSnapshot(A, RunId, Lookup, Snapshot, Error));
    TestEqual(TEXT("Visible candidate is frozen regardless of legacy flag"), Snapshot.IncludedAssets.Num(), 1);
    TestTrue(TEXT("Frozen candidate is marked included"), Snapshot.IncludedAssets[0].bIncluded);
    TestEqual(TEXT("Lookup invoked once"), LookupCalls, 1);
    TestEqual(TEXT("Snapshot belongs to A"), Snapshot.SessionId, A.SessionId);
    A.Candidates[0]->ObjectPath = TEXT("/Game/AgentP2/Changed.Changed");
    A.ModelOptions.Model = TEXT("New Model");
    A.DraftText = TEXT("New draft");
    TestEqual(TEXT("Path is a value copy"), Snapshot.IncludedAssets[0].ObjectPath, FString(TEXT("/Game/AgentP2/Enemy.Enemy")));
    TestEqual(TEXT("Model is frozen"), Snapshot.ModelOptions.Model, FString(TEXT("Model A")));
    TestEqual(TEXT("Input is frozen"), Snapshot.UserInput, FString(TEXT("Inspect Enemy")));
    TestEqual(TEXT("Token limit is frozen"), Snapshot.ModelOptions.MaxOutputTokens, 123);
    FAgentRunInputSnapshot Other;
    TestTrue(TEXT("B can send without candidates"), FAgentAssetContextService::BuildSnapshot(B, FGuid::NewGuid(), Lookup, Other, Error));
    TestEqual(TEXT("B has no A assets"), Other.IncludedAssets.Num(), 0);
    TestEqual(TEXT("No lookup for B"), LookupCalls, 1);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentSnapshotInvalidTest,
    "AgentWorkbench.P2.InvalidCandidates",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentSnapshotInvalidTest::RunTest(const FString& Parameters)
{
    FAgentSession Session;
    Session.DraftText = TEXT("Check path");
    TSharedPtr<FAgentCandidate> Candidate = MakeShared<FAgentCandidate>();
    Candidate->ObjectPath = TEXT("/Engine/Outside.Outside");
    Session.Candidates.Add(Candidate);
    FAgentRunInputSnapshot Snapshot;
    FString Error;
    int32 Calls = 0;
    auto Missing = [&Calls](const FSoftObjectPath&) { ++Calls; FAgentAssetLookupResult R; R.Status = EAgentCandidateValidation::Missing; return R; };
    TestFalse(TEXT("Out-of-root candidate blocks send"), FAgentAssetContextService::BuildSnapshot(Session, FGuid::NewGuid(), Missing, Snapshot, Error));
    TestEqual(TEXT("Invalid path never reaches lookup"), Calls, 0);
    Candidate->ObjectPath = TEXT("/Game/AgentP2/Missing.Missing");
    TestFalse(TEXT("Missing candidate blocks send"), FAgentAssetContextService::BuildSnapshot(Session, FGuid::NewGuid(), Missing, Snapshot, Error));
    TestEqual(TEXT("Missing lookup called"), Calls, 1);
    TestEqual(TEXT("Validation status is visible on candidate"), Candidate->ValidationStatus, EAgentCandidateValidation::Missing);
    TestEqual(TEXT("Failed send preserves draft"), Session.DraftText, FString(TEXT("Check path")));
    auto Scanning = [](const FSoftObjectPath&) { return FAgentAssetLookupResult(); };
    TestFalse(TEXT("Registry scanning blocks send without reporting missing"), FAgentAssetContextService::BuildSnapshot(Session, FGuid::NewGuid(), Scanning, Snapshot, Error));
    TestEqual(TEXT("Scanning status remains unknown"), Candidate->ValidationStatus, EAgentCandidateValidation::Unknown);
    TestTrue(TEXT("Scanning feedback is explicit"), Error.Contains(TEXT("扫描")));
    Candidate->bIncluded = false;
    TestFalse(TEXT("Legacy disabled flag cannot silently hide a visible invalid candidate"),
        FAgentAssetContextService::BuildSnapshot(Session, FGuid::NewGuid(), Missing, Snapshot, Error));
    Session.Candidates.Empty();
    TestTrue(TEXT("Removing invalid candidate permits plain chat"), FAgentAssetContextService::BuildSnapshot(Session, FGuid::NewGuid(), Missing, Snapshot, Error));
    TestEqual(TEXT("Removed candidate omitted"), Snapshot.IncludedAssets.Num(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentReadOnlyToolsTest,
    "AgentWorkbench.P2.ReadOnlyTools",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentReadOnlyToolsTest::RunTest(const FString& Parameters)
{
    const FAssetData Asset = MakeAsset(TEXT("/Game/AgentP2/Enemy"), TEXT("Enemy"), UBlueprint::StaticClass());
    FAgentRunInputSnapshot Snapshot;
    Snapshot.SessionId = FGuid::NewGuid();
    Snapshot.RunId = FGuid::NewGuid();
    FAgentCandidate Candidate;
    Candidate.ObjectPath = Asset.GetSoftObjectPath().ToString();
    Candidate.DisplayName = TEXT("Enemy");
    Snapshot.IncludedAssets.Add(Candidate);
    int32 Calls = 0;
    auto Lookup = [&Asset, &Calls](const FSoftObjectPath&) { ++Calls; return Found(Asset); };
    const TSharedRef<FJsonObject> List = FAgentReadOnlyTools::Execute(Snapshot, TEXT("list_context_assets"), nullptr, Lookup);
    TestTrue(TEXT("List succeeds"), List->GetBoolField(TEXT("ok")));
    TestEqual(TEXT("List contains one authorized candidate"), List->GetArrayField(TEXT("assets")).Num(), 1);
    TestEqual(TEXT("List never reads registry"), Calls, 0);
    TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();
    Args->SetStringField(TEXT("object_path"), TEXT("/Game/AgentP2/Other.Other"));
    TestEqual(TEXT("Other path rejected"), FAgentReadOnlyTools::Execute(Snapshot, TEXT("get_asset_metadata"), Args, Lookup)->GetStringField(TEXT("error_code")), FString(TEXT("UnauthorizedAsset")));
    TestEqual(TEXT("Unauthorized path never reaches lookup"), Calls, 0);
    Args->SetStringField(TEXT("object_path"), Candidate.ObjectPath);
    const TSharedRef<FJsonObject> Metadata = FAgentReadOnlyTools::Execute(Snapshot, TEXT("get_asset_metadata"), Args, Lookup);
    TestTrue(TEXT("Authorized metadata succeeds"), Metadata->GetBoolField(TEXT("ok")));
    TestEqual(TEXT("Metadata path is exact"), Metadata->GetStringField(TEXT("object_path")), Candidate.ObjectPath);
    TestTrue(TEXT("Metadata reports known or unknown modification state"), !Metadata->GetStringField(TEXT("modification_state")).IsEmpty());
    TestEqual(TEXT("Authorized path reads once"), Calls, 1);
    TestEqual(TEXT("Unknown tool denied"), FAgentReadOnlyTools::Execute(Snapshot, TEXT("shell"), nullptr, Lookup)->GetStringField(TEXT("error_code")), FString(TEXT("UnknownTool")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentProjectRegistryTest,
    "AgentWorkbench.P2.ProjectRegistryIntegration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentProjectRegistryTest::RunTest(const FString& Parameters)
{
    IAssetRegistry& Registry = IAssetRegistry::GetChecked();
    Registry.WaitForCompletion();
    TArray<FAssetData> Blueprints;
    Registry.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), Blueprints, true);
    const FAssetData* ProjectBlueprint = Blueprints.FindByPredicate([](const FAssetData& Asset)
    {
        return Asset.PackageName.ToString().StartsWith(TEXT("/Game/"))
            && Asset.IsInstanceOf<UBlueprint>(EResolveClass::No);
    });
    TestNotNull(TEXT("Current project has an indexed Blueprint"), ProjectBlueprint);
    if (!ProjectBlueprint) { return false; }
    FAgentSession Session;
    Session.DraftText = TEXT("Inspect project Blueprint path");
    const FAgentAddCandidatesResult Added = FAgentAssetContextService::AddAssets(Session, {*ProjectBlueprint}, 64);
    TestEqual(TEXT("Real registry Blueprint becomes candidate"), Added.Added, 1);
    FAgentRunInputSnapshot Snapshot;
    FString Error;
    TestTrue(TEXT("Real registry Blueprint validates for request snapshot"), FAgentAssetContextService::BuildSnapshot(
        Session, FGuid::NewGuid(), [](const FSoftObjectPath& Path) { return FAgentAssetContextService::LookupAsset(Path); }, Snapshot, Error));
    if (Snapshot.IncludedAssets.Num() != 1) { return false; }
    TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();
    Args->SetStringField(TEXT("object_path"), Snapshot.IncludedAssets[0].ObjectPath);
    const TSharedRef<FJsonObject> Metadata = FAgentReadOnlyTools::Execute(Snapshot, TEXT("get_asset_metadata"), Args,
        [](const FSoftObjectPath& Path) { return FAgentAssetContextService::LookupAsset(Path); });
    TestTrue(TEXT("Read-only metadata succeeds for real project Blueprint"), Metadata->GetBoolField(TEXT("ok")));
    TestEqual(TEXT("Metadata path matches indexed Blueprint"), Metadata->GetStringField(TEXT("object_path")),
        ProjectBlueprint->GetSoftObjectPath().ToString());
    const bool bSent = Session.BeginRun(Error);
    TestTrue(TEXT("Send snapshot uses validated real project Blueprint"), bSent);
    if (!bSent) { AddError(Error); return false; }
    TestEqual(TEXT("Sent user message retains one asset reference"), Session.Messages[0]->IncludedAssets.Num(), 1);
    TestEqual(TEXT("Run snapshot contains actual object path"),
        Session.Runner.LastSnapshot->IncludedAssets[0].ObjectPath, ProjectBlueprint->GetSoftObjectPath().ToString());
    TestTrue(TEXT("Active Run keeps its frozen tool bindings"),
        !Session.Runner.LastSnapshot->AllowedToolNames.IsEmpty()
        && !Session.Runner.LastSnapshot->AllowedToolHandlerIds.IsEmpty());
    TestEqual(TEXT("Run history receives one input snapshot"), Session.RunHistory.Num(), 1);
    if (Session.RunHistory.Num() == 1)
    {
        TestTrue(TEXT("In-memory history omits tool names"), Session.RunHistory[0].AllowedToolNames.IsEmpty());
        TestTrue(TEXT("In-memory history omits handler bindings"), Session.RunHistory[0].AllowedToolHandlerIds.IsEmpty());
    }
    const FString SentPath = Session.Runner.LastSnapshot->IncludedAssets[0].ObjectPath;
    Session.Candidates[0]->ObjectPath = TEXT("/Game/AgentP2/Changed.Changed");
    TestEqual(TEXT("Editing next-round candidate does not alter completed Run"),
        Session.Runner.LastSnapshot->IncludedAssets[0].ObjectPath, SentPath);
    Session.Candidates.Empty();
    TestEqual(TEXT("Removing candidate clears only Session references"), Session.Candidates.Num(), 0);
    TestEqual(TEXT("Real Blueprint remains in Asset Registry after removing candidate"),
        FAgentAssetContextService::LookupAsset(ProjectBlueprint->GetSoftObjectPath()).Status,
        EAgentCandidateValidation::Valid);
    TArray<FAssetData> ProjectAssets;
    Registry.GetAssetsByPath(FName(TEXT("/Game")), ProjectAssets, true);
    const FAssetData* OtherAsset = ProjectAssets.FindByPredicate([](const FAssetData& Asset)
    {
        FString Path;
        return Asset.IsValid() && !Asset.IsRedirector() && !Asset.AssetClassPath.IsNull()
            && !Asset.IsInstanceOf<UBlueprint>(EResolveClass::No)
            && FAgentAssetContextService::NormalizeObjectPath(Asset.GetSoftObjectPath().ToString(), Path);
    });
    TestNotNull(TEXT("Current project has a non-Blueprint Content asset"), OtherAsset);
    if (!OtherAsset) { return false; }
    FAgentSession GenericSession;
    GenericSession.DraftText = TEXT("Inspect selected Content asset");
    TestEqual(TEXT("Real non-Blueprint asset is accepted"),
        FAgentAssetContextService::AddAssets(GenericSession, {*OtherAsset}, 64).Added, 1);
    FAgentRunInputSnapshot GenericSnapshot;
    TestTrue(TEXT("Real non-Blueprint asset validates for send"), FAgentAssetContextService::BuildSnapshot(
        GenericSession, FGuid::NewGuid(), [](const FSoftObjectPath& Path) { return FAgentAssetContextService::LookupAsset(Path); },
        GenericSnapshot, Error));
    if (GenericSnapshot.IncludedAssets.Num() != 1) { AddError(Error); return false; }
    Args->SetStringField(TEXT("object_path"), GenericSnapshot.IncludedAssets[0].ObjectPath);
    const TSharedRef<FJsonObject> GenericMetadata = FAgentReadOnlyTools::Execute(GenericSnapshot, TEXT("get_asset_metadata"), Args,
        [](const FSoftObjectPath& Path) { return FAgentAssetContextService::LookupAsset(Path); });
    TestTrue(TEXT("Read-only metadata works for real non-Blueprint asset"), GenericMetadata->GetBoolField(TEXT("ok")));
    TestEqual(TEXT("Snapshot includes actual non-Blueprint object path"),
        GenericSnapshot.IncludedAssets[0].ObjectPath, OtherAsset->GetSoftObjectPath().ToString());
    return true;
}

#endif
