#include "AgentAssetViewFollow.h"

#include "AgentAssetContextService.h"
#include "AgentWorkbenchSession.h"
#include "BlueprintEditorModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "GraphEditor.h"
#include "IMaterialEditor.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialFunction.h"
#include "Misc/App.h"
#include "Subsystems/AssetEditorSubsystem.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif

namespace
{
enum class EFollowKind : uint8 { Blueprint, Material };

struct FViewState final : IAgentToolObserverState
{
    virtual bool NeedsVisibleEditorFrame() const override { return true; }
    EFollowKind Kind = EFollowKind::Blueprint;
    TWeakObjectPtr<UObject> Asset;
    FString GraphPath;
    FString ReferencedNodePath;
    FString ReferencedExpressionPath;
    TSet<FString> ExistingGraphPaths;
    TSet<FGuid> ExistingNodeIds;
    TSet<FString> ExistingExpressions;
};

bool IsSupportedEdit(const FString& Name, EFollowKind& OutKind)
{
    constexpr TCHAR BlueprintPrefix[] = TEXT("editor_toolset.toolsets.blueprint.BlueprintTools.");
    constexpr TCHAR MaterialPrefix[] = TEXT("editor_toolset.toolsets.material.MaterialTools.");
    static const TSet<FString> BlueprintEdits = {
        TEXT("add_function_graph"), TEXT("add_event"), TEXT("remove_function_graph"),
        TEXT("add_function_param"), TEXT("remove_function_param"),
        TEXT("add_struct_function_param"), TEXT("add_object_function_param"),
        TEXT("create_node"), TEXT("delete_node"), TEXT("add_node_pin"),
        TEXT("remove_node_pin"), TEXT("retarget_node_class"), TEXT("set_node_position"),
        TEXT("arrange_nodes"), TEXT("connect_pins"), TEXT("break_pins"),
        TEXT("set_pin_value"), TEXT("add_variable"), TEXT("add_struct_variable"),
        TEXT("add_object_variable"), TEXT("set_variable_instance_editable"),
        TEXT("set_variable_replication"), TEXT("set_variable_category"),
        TEXT("remove_variable"), TEXT("add_event_dispatcher"), TEXT("write_graph_dsl")
    };
    static const TSet<FString> MaterialEdits = {
        TEXT("add_expression"), TEXT("delete_expression"), TEXT("layout_expressions"),
        TEXT("rename_parameter_group"), TEXT("delete_parameter_group"),
        TEXT("connect_expressions"), TEXT("disconnect_expressions"),
        TEXT("connect_to_output"), TEXT("disconnect_from_output"),
        TEXT("delete_unused_expressions")
    };
    if (Name.StartsWith(BlueprintPrefix, ESearchCase::CaseSensitive)
        && BlueprintEdits.Contains(Name.RightChop(UE_ARRAY_COUNT(BlueprintPrefix) - 1)))
    {
        OutKind = EFollowKind::Blueprint;
        return true;
    }
    if (Name.StartsWith(MaterialPrefix, ESearchCase::CaseSensitive)
        && MaterialEdits.Contains(Name.RightChop(UE_ARRAY_COUNT(MaterialPrefix) - 1)))
    {
        OutKind = EFollowKind::Material;
        return true;
    }
    return false;
}

void CollectReferences(const TSharedPtr<FJsonValue>& Value, TArray<FString>& Out)
{
    if (!Value) { return; }
    if (Value->Type == EJson::String)
    {
        Out.Add(Value->AsString());
    }
    else if (Value->Type == EJson::Array)
    {
        for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
        { CollectReferences(Item, Out); }
    }
    else if (Value->Type == EJson::Object)
    {
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Value->AsObject()->Values)
        {
            if (Field.Key != TEXT("code")) { CollectReferences(Field.Value, Out); }
        }
    }
}

bool ReferencesPath(const TArray<FString>& References, const FString& Path)
{
    return References.ContainsByPredicate([&Path](const FString& Text)
    {
        int32 Position = Text.Find(Path, ESearchCase::CaseSensitive);
        while (Position != INDEX_NONE)
        {
            const int32 End = Position + Path.Len();
            if (End == Text.Len() || (!FChar::IsAlnum(Text[End]) && Text[End] != TEXT('_')))
            { return true; }
            Position = Text.Find(Path, ESearchCase::CaseSensitive, ESearchDir::FromStart,
                End);
        }
        return false;
    });
}

UObject* FindTargetAsset(const FAgentResolvedToolInvocation& Tool,
    const TArray<FString>& References, EFollowKind Kind)
{
    const FAgentCandidate* Matched = nullptr;
    for (const FAgentCandidate& Candidate : Tool.Invocation.Snapshot->IncludedAssets)
    {
        if (!Candidate.bIncluded || !ReferencesPath(References, Candidate.ObjectPath)) { continue; }
        if (Matched) { return nullptr; } // Ambiguous cross-asset call: do not follow the wrong editor.
        Matched = &Candidate;
    }
    FString TargetPath = Matched ? Matched->ObjectPath : FString();
    if (TargetPath.IsEmpty())
    {
        TSet<FString> Paths;
        for (const FString& Reference : References)
        {
            const FString Path = FSoftObjectPath(Reference).GetAssetPathString();
            if (Path.StartsWith(TEXT("/Game/"))
                && FAgentAssetContextService::LookupAsset(FSoftObjectPath(Path)).Status
                    == EAgentCandidateValidation::Valid)
            { Paths.Add(Path); }
        }
        if (Paths.Num() != 1) { return nullptr; }
        TargetPath = *Paths.CreateConstIterator();
    }
    UObject* Asset = FSoftObjectPath(TargetPath).ResolveObject();
    if (!Asset) { Asset = FSoftObjectPath(TargetPath).TryLoad(); }
    if (Kind == EFollowKind::Blueprint && Cast<UBlueprint>(Asset)) { return Asset; }
    if (Kind == EFollowKind::Material && (Cast<UMaterial>(Asset) || Cast<UMaterialFunction>(Asset)))
    { return Asset; }
    return nullptr;
}

void GetBlueprintGraphs(UBlueprint* Blueprint, TArray<UEdGraph*>& Out)
{
    if (Blueprint) { Blueprint->GetAllGraphs(Out); }
}

IBlueprintEditor* FindBlueprintEditor(UObject* Asset)
{
    UAssetEditorSubsystem* Editors = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
    IAssetEditorInstance* Instance = Editors ? Editors->FindEditorForAsset(Asset, false) : nullptr;
    return Instance && Instance->GetEditorName() == FName(TEXT("BlueprintEditor"))
        ? static_cast<IBlueprintEditor*>(Instance) : nullptr;
}

IMaterialEditor* FindMaterialEditor(UObject* Asset)
{
    UAssetEditorSubsystem* Editors = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
    IAssetEditorInstance* Instance = Editors ? Editors->FindEditorForAsset(Asset, false) : nullptr;
    return Instance && Instance->GetEditorName() == FName(TEXT("MaterialEditor"))
        ? static_cast<IMaterialEditor*>(Instance) : nullptr;
}

TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions(UObject* Asset)
{
    if (UMaterial* Material = Cast<UMaterial>(Asset)) { return Material->GetExpressions(); }
    if (UMaterialFunction* Function = Cast<UMaterialFunction>(Asset)) { return Function->GetExpressions(); }
    return {};
}

UEdGraph* FindReferencedGraph(UBlueprint* Blueprint, const TArray<FString>& References)
{
    TArray<UEdGraph*> Graphs;
    GetBlueprintGraphs(Blueprint, Graphs);
    for (UEdGraph* Graph : Graphs)
    {
        if (Graph && ReferencesPath(References, Graph->GetPathName())) { return Graph; }
    }
    return nullptr;
}
}

TSharedPtr<IAgentToolObserverState> FAgentAssetViewFollow::BeforeExecute(
    const FAgentResolvedToolInvocation& Tool)
{
    check(IsInGameThread());
    if (IsRunningCommandlet() || FApp::IsUnattended() || !GEditor) { return nullptr; }
    EFollowKind Kind;
    if (!IsSupportedEdit(Tool.DisplayName, Kind)) { return nullptr; }
    TArray<FString> References;
    for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Tool.Arguments->Values)
    {
        if (Field.Key != TEXT("code")) { CollectReferences(Field.Value, References); }
    }
    UObject* Asset = FindTargetAsset(Tool, References, Kind);
    UAssetEditorSubsystem* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
    if (!Asset || !Editors) { return nullptr; }
    const TSharedRef<FViewState> State = MakeShared<FViewState>();
    State->Kind = Kind;
    State->Asset = Asset;
    if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
    {
        TArray<UEdGraph*> Graphs;
        GetBlueprintGraphs(Blueprint, Graphs);
        for (UEdGraph* Graph : Graphs)
        {
            if (!Graph) { continue; }
            State->ExistingGraphPaths.Add(Graph->GetPathName());
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (!Node) { continue; }
                State->ExistingNodeIds.Add(Node->NodeGuid);
                if (State->ReferencedNodePath.IsEmpty()
                    && ReferencesPath(References, Node->GetPathName()))
                {
                    State->ReferencedNodePath = Node->GetPathName();
                    State->GraphPath = Graph->GetPathName();
                }
            }
        }
        if (State->GraphPath.IsEmpty())
        {
            if (UEdGraph* ReferencedGraph = FindReferencedGraph(Blueprint, References))
            { State->GraphPath = ReferencedGraph->GetPathName(); }
        }
    }
    else
    {
        for (UMaterialExpression* Expression : Expressions(Asset))
        {
            if (!Expression) { continue; }
            State->ExistingExpressions.Add(Expression->GetPathName());
            if (State->ReferencedExpressionPath.IsEmpty()
                && ReferencesPath(References, Expression->GetPathName()))
            { State->ReferencedExpressionPath = Expression->GetPathName(); }
        }
    }
    if (!Editors->FindEditorForAsset(Asset, false))
    { Editors->OpenEditorForAsset(Asset, EToolkitMode::Standalone, {}, false); }
    if (UBlueprint* Blueprint = Cast<UBlueprint>(Asset))
    {
        if (IBlueprintEditor* Editor = FindBlueprintEditor(Blueprint))
        {
            Editor->FocusWindow();
            TArray<UEdGraph*> Graphs;
            GetBlueprintGraphs(Blueprint, Graphs);
            for (UEdGraph* Graph : Graphs)
            {
                if (Graph && Graph->GetPathName() == State->GraphPath)
                { Editor->OpenGraphAndBringToFront(Graph); break; }
            }
        }
    }
    else if (IMaterialEditor* Editor = FindMaterialEditor(Asset))
    {
        Editor->FocusWindow();
    }
    return State;
}

void FAgentAssetViewFollow::AfterExecute(const FAgentResolvedToolInvocation& Tool,
    const TSharedPtr<IAgentToolObserverState>& InState, const FMCPToolResult& Result)
{
    check(IsInGameThread());
    if (Result.bIsError || !InState || Tool.Invocation.CancellationToken->load()) { return; }
    const TSharedPtr<FViewState> State = StaticCastSharedPtr<FViewState>(InState);
    UObject* Asset = State->Asset.Get();
    if (!Asset) { return; }
    if (State->Kind == EFollowKind::Blueprint)
    {
        UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
        IBlueprintEditor* Editor = FindBlueprintEditor(Asset);
        if (!Blueprint || !Editor) { return; }
        TArray<UEdGraph*> Graphs;
        GetBlueprintGraphs(Blueprint, Graphs);
        if (Tool.DisplayName == TEXT("editor_toolset.toolsets.blueprint.BlueprintTools.write_graph_dsl"))
        {
            // write_graph_dsl compiles before returning. Its nodes may be reinstanced again
            // by a subsequent compile, so do not leave a deferred node jump targeting one.
            for (UEdGraph* Graph : Graphs)
            {
                if (Graph && Graph->GetPathName() == State->GraphPath)
                { Editor->OpenGraphAndBringToFront(Graph); break; }
            }
            return;
        }
        UEdGraph* TargetGraph = nullptr;
        UEdGraphNode* TargetNode = nullptr;
        for (UEdGraph* Graph : Graphs)
        {
            if (!Graph) { continue; }
            if (Graph->GetPathName() == State->GraphPath) { TargetGraph = Graph; }
            if (!State->ExistingGraphPaths.Contains(Graph->GetPathName()) && !TargetGraph)
            { TargetGraph = Graph; }
            for (UEdGraphNode* Node : Graph->Nodes)
            {
                if (Node && !State->ExistingNodeIds.Contains(Node->NodeGuid)
                    && (!TargetNode || Graph->GetPathName() == State->GraphPath))
                { TargetGraph = Graph; TargetNode = Node; }
                else if (Node && !TargetNode && Node->GetPathName() == State->ReferencedNodePath)
                { TargetGraph = Graph; TargetNode = Node; }
            }
        }
        if (!TargetGraph) { return; }
        // EditorToolset already changed the graph (and write_graph_dsl compiled it).
        // A second notification here can schedule more Blueprint reconstruction.
        if (TSharedPtr<SGraphEditor> GraphEditor = Editor->OpenGraphAndBringToFront(TargetGraph))
        {
            if (TargetNode) { GraphEditor->JumpToNode(TargetNode, false, true); }
        }
    }
    else if (IMaterialEditor* Editor = FindMaterialEditor(Asset))
    {
        // EditorToolset's MaterialTools already refreshes its preview after each edit.
        // RefreshGraphFromOriginal here would discard pending edits a second time.
        Editor->FocusWindow();
        UMaterialExpression* Added = nullptr;
        for (UMaterialExpression* Expression : Expressions(Asset))
        {
            if (Expression && !State->ExistingExpressions.Contains(Expression->GetPathName()))
            { Added = Expression; break; }
        }
        if (!Added)
        {
            for (UMaterialExpression* Expression : Expressions(Asset))
            {
                if (Expression && Expression->GetPathName() == State->ReferencedExpressionPath)
                { Added = Expression; break; }
            }
        }
        if (!Added) { return; }
        const FGuid AddedId = Added->GetMaterialExpressionId();
        UAssetEditorSubsystem* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
        for (UObject* EditedObject : Editors->GetAllEditedAssets())
        {
            if (EditedObject == Asset || Editors->FindEditorForAsset(EditedObject, false) != Editor)
            { continue; }
            for (UMaterialExpression* Preview : Expressions(EditedObject))
            {
                if (Preview && Preview->GraphNode && Preview->GetClass() == Added->GetClass()
                    && Preview->GetMaterialExpressionId() == AddedId
                    && Preview->GetName() == Added->GetName()
                    && Preview->MaterialExpressionEditorX == Added->MaterialExpressionEditorX
                    && Preview->MaterialExpressionEditorY == Added->MaterialExpressionEditorY)
                {
                    Editor->JumpToExpression(Preview);
                    Editor->AddToSelection(Preview);
                    return;
                }
            }
        }
    }
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentAssetViewFollowRoutingTest,
    "AgentWorkbench.ToolWorkflow.AssetViewFollowRouting",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentAssetViewFollowRoutingTest::RunTest(const FString& Parameters)
{
    EFollowKind Kind = EFollowKind::Blueprint;
    TestTrue(TEXT("Blueprint node edit follows"), IsSupportedEdit(
        TEXT("editor_toolset.toolsets.blueprint.BlueprintTools.create_node"), Kind));
    TestTrue(TEXT("Blueprint edit type"), Kind == EFollowKind::Blueprint);
    TestTrue(TEXT("Material expression edit follows"), IsSupportedEdit(
        TEXT("editor_toolset.toolsets.material.MaterialTools.add_expression"), Kind));
    TestTrue(TEXT("Material edit type"), Kind == EFollowKind::Material);
    TestFalse(TEXT("Blueprint read stays passive"), IsSupportedEdit(
        TEXT("editor_toolset.toolsets.blueprint.BlueprintTools.list_graphs"), Kind));
    TestFalse(TEXT("Other toolset does not trigger the editor"), IsSupportedEdit(
        TEXT("other.BlueprintTools.create_node"), Kind));
    TestFalse(TEXT("Name with a prefixed toolset is ignored"), IsSupportedEdit(
        TEXT("other.editor_toolset.toolsets.blueprint.BlueprintTools.create_node"), Kind));
    TestTrue(TEXT("Exact candidate object path is found in graph reference"), ReferencesPath(
        {TEXT("/Game/Folder/BP.BP:EventGraph")}, TEXT("/Game/Folder/BP.BP")));
    TestFalse(TEXT("Unrelated asset is not followed"), ReferencesPath(
        {TEXT("/Game/Folder/Other.Other:EventGraph")}, TEXT("/Game/Folder/BP.BP")));
    TestFalse(TEXT("A path prefix does not select another asset"), ReferencesPath(
        {TEXT("/Game/Folder/BP.BPExtra:EventGraph")}, TEXT("/Game/Folder/BP.BP")));
    const TSharedRef<FJsonObject> GraphReference = MakeShared<FJsonObject>();
    GraphReference->SetStringField(TEXT("refPath"), TEXT("/Game/Folder/BP.BP:EventGraph"));
    TArray<FString> NestedReferences;
    CollectReferences(MakeShared<FJsonValueObject>(GraphReference), NestedReferences);
    TestTrue(TEXT("Toolset graph refPath identifies its asset before dispatch"),
        ReferencesPath(NestedReferences, TEXT("/Game/Folder/BP.BP")));
    return true;
}
#endif
