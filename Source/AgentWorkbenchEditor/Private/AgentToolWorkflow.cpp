#include "AgentToolWorkflow.h"
#include "AgentToolBridge.h"
#include "AgentUnrealMCPBridge.h"
#include "AgentAssetViewFollow.h"
#include "AgentWorkbenchDisplayFormatter.h"
#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "CoreGlobals.h"
#include "Serialization/JsonSerializer.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "UObject/SoftObjectPath.h"
#include <atomic>

struct FAgentCompilationTracker
{
    TMap<FGuid, FString> AutomaticallyCompiledBlueprints;
};

namespace
{
constexpr TCHAR WriteGraphDslName[] = TEXT("editor_toolset.toolsets.blueprint.BlueprintTools.write_graph_dsl");
constexpr TCHAR ReadGraphDslName[] = TEXT("editor_toolset.toolsets.blueprint.BlueprintTools.read_graph_dsl");
constexpr TCHAR CompileBlueprintName[] = TEXT("editor_toolset.toolsets.blueprint.BlueprintTools.compile_blueprint");

FString ReferencedAssetPath(const FJsonObject& Arguments, const TCHAR* Key)
{
    const TSharedPtr<FJsonObject>* Reference = nullptr;
    FString ReferencePath;
    if (!Arguments.TryGetObjectField(Key, Reference) || !Reference || !Reference->IsValid()
        || !(*Reference)->TryGetStringField(TEXT("refPath"), ReferencePath))
    { return FString(); }
    return FSoftObjectPath(ReferencePath).GetAssetPathString();
}

bool IsRedundantBlueprintCompile(const FAgentCompilationTracker& Tracker, const FGuid& RunId,
    const FString& DisplayName, const FJsonObject& Arguments)
{
    if (DisplayName != CompileBlueprintName) { return false; }
    bool bWarningsAsErrors = false;
    if (Arguments.TryGetBoolField(TEXT("warnings_as_errors"), bWarningsAsErrors)
        && bWarningsAsErrors) { return false; }
    const FString* CompiledPath = Tracker.AutomaticallyCompiledBlueprints.Find(RunId);
    return CompiledPath && !CompiledPath->IsEmpty()
        && *CompiledPath == ReferencedAssetPath(Arguments, TEXT("blueprint"));
}

void RecordBlueprintCompilation(FAgentCompilationTracker& Tracker, const FGuid& RunId,
    const FString& DisplayName, const FJsonObject& Arguments, const FMCPToolResult& Result)
{
    if (DisplayName == WriteGraphDslName)
    {
        const FString Path = ReferencedAssetPath(Arguments, TEXT("graph"));
        if (!Result.bIsError && !Path.IsEmpty())
        { Tracker.AutomaticallyCompiledBlueprints.Add(RunId, Path); }
        else { Tracker.AutomaticallyCompiledBlueprints.Remove(RunId); }
    }
    else if (DisplayName != ReadGraphDslName
        && DisplayName != TEXT("ue_mcp.catalog.search")
        && DisplayName != TEXT("ue_mcp.catalog.describe")
        && DisplayName != CompileBlueprintName)
    {
        // Any other tool may have changed the Blueprint since the automatic compile.
        Tracker.AutomaticallyCompiledBlueprints.Remove(RunId);
    }
}

struct FWorkflowCall
{
    FAgentToolInvocation Invocation;
    TArray<TSharedRef<IAgentToolGate>> Gates;
    TArray<TSharedRef<IAgentToolExecutionObserver>> Observers;
    TSharedRef<FAgentCompilationTracker> CompilationTracker;
    double ToolTimeoutSeconds;
    TSharedPtr<FJsonObject> Arguments;
    TFunction<void(FMCPToolResult)> Complete;
    bool bFinished = false; // Game Thread only.

    FWorkflowCall(const FAgentToolInvocation& InInvocation,
        const TArray<TSharedRef<IAgentToolGate>>& InGates,
        const TArray<TSharedRef<IAgentToolExecutionObserver>>& InObservers,
        const TSharedRef<FAgentCompilationTracker>& InCompilationTracker, double InTimeoutSeconds,
        TFunction<void(FMCPToolResult)> InComplete)
        : Invocation(InInvocation), Gates(InGates), Observers(InObservers),
          CompilationTracker(InCompilationTracker),
          ToolTimeoutSeconds(InTimeoutSeconds),
          Complete(MoveTemp(InComplete)) {}
};

bool ConfirmUnrealMCPTool(const FString& ToolName, const FString& ArgumentsJson,
    FString& OutReason)
{
    check(IsInGameThread());
    if (FApp::IsUnattended() || GIsRunningUnattendedScript
        || !FSlateApplication::IsInitialized()
        || !FSlateApplication::Get().CanAddModalWindow())
    {
        OutReason = TEXT("Tool approval requires an interactive editor window.");
        return false;
    }

    FAgentEvent DetailEvent;
    DetailEvent.Type = EAgentEventType::ToolCallStarted;
    DetailEvent.Detail = ArgumentsJson;
    const FString DisplayArguments =
        FAgentWorkbenchDisplayFormatter::FormatEventDetail(DetailEvent);
    // A truncated preview cannot be approved as if it were the full call.
    if (ArgumentsJson.Len() > 32768 || DisplayArguments.Len() > 32768)
    {
        OutReason = TEXT("Tool arguments are too large to display completely for approval.");
        return false;
    }

    const TSharedRef<bool> Approved = MakeShared<bool>(false);
    const TSharedRef<SWindow> Dialog = SNew(SWindow)
        .Title(FText::FromString(TEXT("批准 Unreal MCP 工具调用")))
        .ClientSize(FVector2D(800.0f, 520.0f))
        .SupportsMinimize(false)
        .SupportsMaximize(true);
    const TWeakPtr<SWindow> WeakDialog = Dialog;
    Dialog->SetContent(
        SNew(SBorder).Padding(12.0f)
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 8.0f)
            [
                SNew(STextBlock)
                .Text(FText::FromString(FString::Printf(
                    TEXT("模型请求执行工具 %s。请核对完整参数后决定是否继续。"),
                    *ToolName)))
                .AutoWrapText(true)
            ]
            + SVerticalBox::Slot().FillHeight(1.0f)
            [
                SNew(SMultiLineEditableTextBox)
                .Text(FText::FromString(DisplayArguments))
                .IsReadOnly(true)
            ]
            + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(0.0f, 10.0f, 0.0f, 0.0f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().Padding(4.0f, 0.0f)
                [
                    SNew(SButton)
                    .Text(FText::FromString(TEXT("拒绝")))
                    .OnClicked_Lambda([WeakDialog]()
                    {
                        if (const TSharedPtr<SWindow> Window = WeakDialog.Pin())
                        { Window->RequestDestroyWindow(); }
                        return FReply::Handled();
                    })
                ]
                + SHorizontalBox::Slot().AutoWidth().Padding(4.0f, 0.0f)
                [
                    SNew(SButton)
                    .Text(FText::FromString(TEXT("允许本次调用")))
                    .OnClicked_Lambda([WeakDialog, Approved]()
                    {
                        *Approved = true;
                        if (const TSharedPtr<SWindow> Window = WeakDialog.Pin())
                        { Window->RequestDestroyWindow(); }
                        return FReply::Handled();
                    })
                ]
            ]
        ]);
    // Unlike FMessageDialog, this window does not copy tool parameters to the UE log.
    FSlateApplication::Get().AddModalWindow(Dialog, nullptr);
    if (!*Approved) { OutReason = TEXT("Tool call was not approved."); }
    return *Approved;
}

bool IsExplicitlyReadOnlyTool(const FString& DisplayName)
{
    // Only exact, locally reviewed tool names may skip the prompt in Smart mode.
    return DisplayName == TEXT("editor_toolset.toolsets.blueprint.BlueprintTools.list_graphs")
        || DisplayName == TEXT("editor_toolset.toolsets.blueprint.BlueprintTools.list_variables")
        || DisplayName == TEXT("AgentSkillToolset.ListSkills");
}

bool ShouldAutomaticallyApprove(EAgentApprovalMode Mode, const FString& TargetName,
    const FString& DisplayName)
{
    return Mode == EAgentApprovalMode::Unrestricted
        || (Mode == EAgentApprovalMode::Smart
            && FAgentUnrealMCPBridge::IsUnrealToolName(TargetName)
            && IsExplicitlyReadOnlyTool(DisplayName));
}

void Finish(const TSharedRef<FWorkflowCall>& Work, FMCPToolResult Result)
{
    check(IsInGameThread());
    if (Work->bFinished) { return; }
    Work->bFinished = true;
    TFunction<void(FMCPToolResult)> Complete = MoveTemp(Work->Complete);
    Complete(MoveTemp(Result));
}

void Advance(const TSharedRef<FWorkflowCall>& Work, int32 GateIndex)
{
    check(IsInGameThread());
    if (Work->bFinished) { return; }
    if (Work->Invocation.CancellationToken->load())
    {
        Finish(Work, FMCPToolResult::Failure(TEXT("Run was cancelled before tool execution.")));
        return;
    }
    if (Work->Invocation.RunDeadlineSeconds > 0.0
        && FPlatformTime::Seconds() >= Work->Invocation.RunDeadlineSeconds)
    {
        Finish(Work, FMCPToolResult::Failure(TEXT("Run exceeded its total timeout before tool execution.")));
        return;
    }
    if (!Work->Arguments)
    {
        const FString ArgumentsText = Work->Invocation.Call.ArgumentsJson.IsEmpty()
            ? TEXT("{}") : Work->Invocation.Call.ArgumentsJson;
        if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ArgumentsText), Work->Arguments)
            || !Work->Arguments)
        {
            Finish(Work, FMCPToolResult::ProtocolError(-32602,
                TEXT("Model tool arguments must be a JSON object.")));
            return;
        }
    }
    if (GateIndex >= Work->Gates.Num())
    {
        FString TargetName;
        TSharedPtr<FJsonObject> TargetArguments;
        FString TargetError;
        if (!FAgentToolBridge::ResolveApprovalTarget(*Work->Invocation.Snapshot,
            Work->Invocation.Call.Name, Work->Arguments.ToSharedRef(),
            TargetName, TargetArguments, TargetError) || !TargetArguments)
        {
            Finish(Work, FMCPToolResult::ProtocolError(-32602,
                TargetError.IsEmpty() ? TEXT("Tool target is invalid.") : TargetError));
            return;
        }
        const FString DisplayName = FAgentToolBridge::GetDisplayName(
            *Work->Invocation.Snapshot, TargetName);
        if (IsRedundantBlueprintCompile(*Work->CompilationTracker,
            Work->Invocation.Snapshot->RunId, DisplayName, *TargetArguments))
        {
            Finish(Work, FMCPToolResult::Success(
                TEXT("Blueprint was already compiled by the successful write_graph_dsl call in this run. No second compilation was performed.")));
            return;
        }
        const bool bRequiresApproval = FAgentToolBridge::RequiresApproval(
            *Work->Invocation.Snapshot, Work->Invocation.Call.Name);
        bool bApproved = false;
        if (bRequiresApproval)
        {
            FString Error;
            bApproved = ShouldAutomaticallyApprove(Work->Invocation.Snapshot->ApprovalMode,
                TargetName, DisplayName);
            if (!bApproved)
            {
                FString TargetArgumentsJson;
                FJsonSerializer::Serialize(TargetArguments.ToSharedRef(),
                    TJsonWriterFactory<>::Create(&TargetArgumentsJson));
                bApproved = ConfirmUnrealMCPTool(DisplayName, TargetArgumentsJson, Error);
            }
            if (!bApproved)
            {
                Finish(Work, FMCPToolResult::Failure(Error));
                return;
            }
        }
        if (Work->Invocation.CancellationToken->load())
        {
            Finish(Work, FMCPToolResult::Failure(TEXT("Run was cancelled before tool execution.")));
            return;
        }
        const double RemainingSeconds = Work->Invocation.RunDeadlineSeconds > 0.0
            ? FMath::Min(Work->ToolTimeoutSeconds,
                Work->Invocation.RunDeadlineSeconds - FPlatformTime::Seconds())
            : Work->ToolTimeoutSeconds;
        if (RemainingSeconds <= 0.0)
        {
            Finish(Work, FMCPToolResult::Failure(TEXT("Run exceeded its total timeout before tool execution.")));
            return;
        }
        const FAgentResolvedToolInvocation Resolved{Work->Invocation, TargetName,
            DisplayName, TargetArguments.ToSharedRef()};
        TArray<TSharedPtr<IAgentToolObserverState>> ObserverStates;
        ObserverStates.Reserve(Work->Observers.Num());
        for (const TSharedRef<IAgentToolExecutionObserver>& Observer : Work->Observers)
        {
            ObserverStates.Add(Observer->BeforeExecute(Resolved));
        }
        const bool bNeedsVisibleFrame = ObserverStates.ContainsByPredicate(
            [](const TSharedPtr<IAgentToolObserverState>& State)
            { return State && State->NeedsVisibleEditorFrame(); });
        // UE toolsets can synchronously mutate or reinstance UObjects. A modal approval
        // window runs its own Slate loop with the engine tick paused, so dispatching here
        // would run inside the suspended outer frame. Yield through the core ticker even
        // in Unrestricted mode, where no approval window is shown.
        const bool bDeferUntilEngineFrame = bNeedsVisibleFrame
            || (FAgentUnrealMCPBridge::IsUnrealToolName(TargetName)
                && !TargetName.StartsWith(TEXT("ue_mcp.catalog."), ESearchCase::CaseSensitive));
        auto Dispatch = [Work, bApproved, TargetName = MoveTemp(TargetName), DisplayName,
            TargetArguments = TargetArguments.ToSharedRef(),
            ObserverStates = MoveTemp(ObserverStates)]() mutable
        {
            if (Work->bFinished) { return; }
            const double DispatchSeconds = Work->Invocation.RunDeadlineSeconds > 0.0
                ? FMath::Min(Work->ToolTimeoutSeconds,
                    Work->Invocation.RunDeadlineSeconds - FPlatformTime::Seconds())
                : Work->ToolTimeoutSeconds;
            if (DispatchSeconds <= 0.0 || Work->Invocation.CancellationToken->load())
            {
                Finish(Work, FMCPToolResult::Failure(TEXT("Run stopped before tool execution.")));
                return;
            }
            const FAgentResolvedToolInvocation Dispatching{Work->Invocation, TargetName,
                DisplayName, TargetArguments};
            for (int32 Index = 0; Index < Work->Observers.Num(); ++Index)
            {
                Work->Observers[Index]->BeforeDispatch(Dispatching, ObserverStates[Index]);
            }
            FAgentToolBridge::Execute(*Work->Invocation.Snapshot, Work->Invocation.Call.Name,
                Work->Arguments.ToSharedRef(), DispatchSeconds, bApproved,
                [Work, TargetName = MoveTemp(TargetName), DisplayName,
                    TargetArguments, ObserverStates = MoveTemp(ObserverStates)](FMCPToolResult Result) mutable
                {
                    if (Work->bFinished) { return; }
                    RecordBlueprintCompilation(*Work->CompilationTracker,
                        Work->Invocation.Snapshot->RunId, DisplayName,
                        *TargetArguments, Result);
                    const FAgentResolvedToolInvocation Completed{Work->Invocation, TargetName,
                        DisplayName, TargetArguments};
                    for (int32 Index = 0; Index < Work->Observers.Num(); ++Index)
                    {
                        Work->Observers[Index]->AfterExecute(Completed, ObserverStates[Index], Result);
                    }
                    Finish(Work, MoveTemp(Result));
                });
        };
        if (bDeferUntilEngineFrame)
        {
            // The first core-ticker pass ends the current engine frame. The second runs
            // after the next GEngine->Tick, outside the modal window's nested Slate loop.
            // This also gives newly opened asset editors a chance to paint.
            FTSTicker::GetCoreTicker().AddTicker(TEXT("AgentWorkbenchDeferredTool"), 0.0f,
                [Dispatch = MoveTemp(Dispatch), FramesToWait = 2](float) mutable
                {
                    if (--FramesToWait > 0) { return true; }
                    Dispatch();
                    return false;
                });
        }
        else { Dispatch(); }
        return;
    }
    const TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> Continued =
        MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
    Work->Gates[GateIndex]->Evaluate(Work->Invocation,
        [Work, GateIndex, Continued](bool bAllow, FString Reason)
        {
            if (Continued->exchange(true)) { return; }
            AsyncTask(ENamedThreads::GameThread,
                [Work, GateIndex, bAllow, Reason = MoveTemp(Reason)]() mutable
                {
                    if (Work->bFinished) { return; }
                    if (!bAllow)
                    {
                        Finish(Work, FMCPToolResult::Failure(Reason.IsEmpty()
                            ? TEXT("Tool call was rejected before execution.") : Reason));
                        return;
                    }
                    Advance(Work, GateIndex + 1);
                });
        });
}
}

FAgentToolWorkflow::FAgentToolWorkflow()
    : CompilationTracker(MakeShared<FAgentCompilationTracker>())
{
    Observers.Add(MakeShared<FAgentAssetViewFollow>());
}

void FAgentToolWorkflow::AddGate(const TSharedRef<IAgentToolGate>& Gate)
{
    check(IsInGameThread());
    Gates.Add(Gate);
}

void FAgentToolWorkflow::AddObserver(const TSharedRef<IAgentToolExecutionObserver>& Observer)
{
    check(IsInGameThread());
    Observers.Add(Observer);
}

void FAgentToolWorkflow::Execute(const FAgentToolInvocation& Invocation, double TimeoutSeconds,
    TFunction<void(FMCPToolResult)> Complete) const
{
    check(IsInGameThread());
    const TSharedRef<FWorkflowCall> Work = MakeShared<FWorkflowCall>(Invocation, Gates, Observers,
        CompilationTracker.ToSharedRef(), TimeoutSeconds, MoveTemp(Complete));
    Advance(Work, 0);
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentToolWorkflowRedundantBlueprintCompileTest,
    "AgentWorkbench.ToolWorkflow.RedundantBlueprintCompile",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentToolWorkflowRedundantBlueprintCompileTest::RunTest(const FString& Parameters)
{
    FAgentCompilationTracker Tracker;
    const FGuid RunId = FGuid::NewGuid();
    const FGuid OtherRunId = FGuid::NewGuid();
    const TSharedRef<FJsonObject> Graph = MakeShared<FJsonObject>();
    Graph->SetStringField(TEXT("refPath"), TEXT("/Game/TopDown/Input/TEST.TEST:EventGraph"));
    const TSharedRef<FJsonObject> WriteArguments = MakeShared<FJsonObject>();
    WriteArguments->SetObjectField(TEXT("graph"), Graph);
    const TSharedRef<FJsonObject> Blueprint = MakeShared<FJsonObject>();
    Blueprint->SetStringField(TEXT("refPath"), TEXT("/Game/TopDown/Input/TEST.TEST"));
    const TSharedRef<FJsonObject> CompileArguments = MakeShared<FJsonObject>();
    CompileArguments->SetObjectField(TEXT("blueprint"), Blueprint);
    TestFalse(TEXT("No write means compile is needed"),
        IsRedundantBlueprintCompile(Tracker, RunId, CompileBlueprintName, *CompileArguments));
    RecordBlueprintCompilation(Tracker, RunId, WriteGraphDslName, *WriteArguments,
        FMCPToolResult::Success(TEXT("written")));
    RecordBlueprintCompilation(Tracker, RunId, ReadGraphDslName, *WriteArguments,
        FMCPToolResult::Success(TEXT("read")));
    TestTrue(TEXT("Successful write already compiled the same Blueprint"),
        IsRedundantBlueprintCompile(Tracker, RunId, CompileBlueprintName, *CompileArguments));
    TestFalse(TEXT("Another run does not inherit a compile"),
        IsRedundantBlueprintCompile(Tracker, OtherRunId, CompileBlueprintName, *CompileArguments));
    Blueprint->SetStringField(TEXT("refPath"), TEXT("/Game/TopDown/Input/Other.Other"));
    TestFalse(TEXT("Another Blueprint still needs compilation"),
        IsRedundantBlueprintCompile(Tracker, RunId, CompileBlueprintName, *CompileArguments));
    Blueprint->SetStringField(TEXT("refPath"), TEXT("/Game/TopDown/Input/TEST.TEST"));
    CompileArguments->SetBoolField(TEXT("warnings_as_errors"), true);
    TestFalse(TEXT("Warning validation still requires the requested compile"),
        IsRedundantBlueprintCompile(Tracker, RunId, CompileBlueprintName, *CompileArguments));
    CompileArguments->RemoveField(TEXT("warnings_as_errors"));
    RecordBlueprintCompilation(Tracker, RunId,
        TEXT("editor_toolset.toolsets.blueprint.BlueprintTools.set_pin_value"),
        *WriteArguments, FMCPToolResult::Success(TEXT("edited")));
    TestFalse(TEXT("A later edit requires a new compile"),
        IsRedundantBlueprintCompile(Tracker, RunId, CompileBlueprintName, *CompileArguments));
    RecordBlueprintCompilation(Tracker, RunId, WriteGraphDslName, *WriteArguments,
        FMCPToolResult::Failure(TEXT("write failed")));
    TestFalse(TEXT("Failed write never records a compile"),
        IsRedundantBlueprintCompile(Tracker, RunId, CompileBlueprintName, *CompileArguments));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentToolApprovalModeTest,
    "AgentWorkbench.ToolWorkflow.ApprovalModes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentToolApprovalModeTest::RunTest(const FString& Parameters)
{
    const FString ReadOnly = TEXT("editor_toolset.toolsets.blueprint.BlueprintTools.list_variables");
    const FString Edit = TEXT("editor_toolset.toolsets.blueprint.BlueprintTools.create_node");
    const FString Target = TEXT("ue_mcp.tool_123");
    TestFalse(TEXT("Ask prompts for read-only UE tools"),
        ShouldAutomaticallyApprove(EAgentApprovalMode::Ask, Target, ReadOnly));
    TestTrue(TEXT("Smart approves reviewed read-only UE tools"),
        ShouldAutomaticallyApprove(EAgentApprovalMode::Smart, Target, ReadOnly));
    TestFalse(TEXT("Smart prompts for asset edits"),
        ShouldAutomaticallyApprove(EAgentApprovalMode::Smart, Target, Edit));
    TestFalse(TEXT("Smart does not trust a native tool with the same display name"),
        ShouldAutomaticallyApprove(EAgentApprovalMode::Smart, TEXT("bsharness.assets.open"), ReadOnly));
    TestTrue(TEXT("Unrestricted approves asset edits"),
        ShouldAutomaticallyApprove(EAgentApprovalMode::Unrestricted, Target, Edit));
    return true;
}

namespace
{
class FDeferredTestGate final : public IAgentToolGate
{
public:
    FAgentToolGateContinuation Pending;
    virtual void Evaluate(const FAgentToolInvocation&, FAgentToolGateContinuation Continue) override
    { Pending = MoveTemp(Continue); }
};

class FCountingObserver final : public IAgentToolExecutionObserver
{
public:
    int32 BeforeCount = 0;
    int32 AfterCount = 0;
    FString SeenName;
    virtual TSharedPtr<IAgentToolObserverState> BeforeExecute(
        const FAgentResolvedToolInvocation& Tool) override
    {
        ++BeforeCount;
        SeenName = Tool.RegistryName;
        return nullptr;
    }
    virtual void AfterExecute(const FAgentResolvedToolInvocation& Tool,
        const TSharedPtr<IAgentToolObserverState>&, const FMCPToolResult&) override
    {
        ++AfterCount;
        SeenName = Tool.RegistryName;
    }
};

class FVisibleFrameState final : public IAgentToolObserverState
{
public:
    virtual bool NeedsVisibleEditorFrame() const override { return true; }
};

class FVisibleFrameObserver final : public IAgentToolExecutionObserver
{
public:
    int32 BeforeCount = 0;
    int32 AfterCount = 0;
    virtual TSharedPtr<IAgentToolObserverState> BeforeExecute(
        const FAgentResolvedToolInvocation&) override
    {
        ++BeforeCount;
        return MakeShared<FVisibleFrameState>();
    }
    virtual void AfterExecute(const FAgentResolvedToolInvocation&,
        const TSharedPtr<IAgentToolObserverState>&, const FMCPToolResult&) override
    { ++AfterCount; }
};

class FDispatchFrameObserver final : public IAgentToolExecutionObserver
{
public:
    uint64 DispatchFrame = 0;
    virtual TSharedPtr<IAgentToolObserverState> BeforeExecute(
        const FAgentResolvedToolInvocation&) override { return nullptr; }
    virtual void BeforeDispatch(const FAgentResolvedToolInvocation&,
        const TSharedPtr<IAgentToolObserverState>&) override { DispatchFrame = GFrameCounter; }
    virtual void AfterExecute(const FAgentResolvedToolInvocation&,
        const TSharedPtr<IAgentToolObserverState>&, const FMCPToolResult&) override {}
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentToolWorkflowVisibleEditorFrameTest,
    "AgentWorkbench.ToolWorkflow.VisibleEditorBeforeDispatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentToolWorkflowVisibleEditorFrameTest::RunTest(const FString& Parameters)
{
    const TSharedRef<FAgentToolWorkflow> Workflow = MakeShared<FAgentToolWorkflow>();
    const TSharedRef<FVisibleFrameObserver> Observer = MakeShared<FVisibleFrameObserver>();
    Workflow->AddObserver(Observer);
    const TSharedRef<FAgentRunInputSnapshot> Snapshot = MakeShared<FAgentRunInputSnapshot>();
    FAgentToolBridge::FreezeAvailableTools(*Snapshot);
    Snapshot->bToolListFrozen = true;
    const TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> Cancelled =
        MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
    FAgentToolCall Call;
    Call.Id = TEXT("visible-frame-call");
    Call.Name = TEXT("ue_mcp.catalog.search");
    Call.ArgumentsJson = TEXT("{\"query\":\"BlueprintTools\",\"limit\":1}");
    struct FObserved { bool bCompleted = false; bool bFailed = false; };
    const TSharedRef<FObserved> Observed = MakeShared<FObserved>();
    Workflow->Execute({Snapshot, Cancelled, FGuid::NewGuid(), Call}, 5.0,
        [Observed](FMCPToolResult Result)
        {
            Observed->bCompleted = true;
            Observed->bFailed = Result.bIsError;
        });
    TestEqual(TEXT("View is prepared before dispatch"), Observer->BeforeCount, 1);
    TestFalse(TEXT("Dispatch waits for an editor frame"), Observed->bCompleted);
    const double Deadline = FPlatformTime::Seconds() + 3.0;
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Observer, Observed, Deadline]()
    {
        if (!Observed->bCompleted && FPlatformTime::Seconds() < Deadline) { return false; }
        TestTrue(TEXT("Tool completes after the frame"), Observed->bCompleted);
        TestFalse(TEXT("Deferred tool succeeds"), Observed->bFailed);
        TestEqual(TEXT("Post-execution observer runs"), Observer->AfterCount, 1);
        return true;
    }));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentToolWorkflowUnrealToolAfterEngineFrameTest,
    "AgentWorkbench.ToolWorkflow.UnrealToolAfterEngineFrame",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentToolWorkflowUnrealToolAfterEngineFrameTest::RunTest(const FString& Parameters)
{
    const TSharedRef<FAgentToolWorkflow> Workflow = MakeShared<FAgentToolWorkflow>();
    const TSharedRef<FDispatchFrameObserver> Observer = MakeShared<FDispatchFrameObserver>();
    Workflow->AddObserver(Observer);
    const TSharedRef<FAgentRunInputSnapshot> Snapshot = MakeShared<FAgentRunInputSnapshot>();
    FAgentToolBridge::FreezeAvailableTools(*Snapshot);
    Snapshot->bToolListFrozen = true;
    Snapshot->ApprovalMode = EAgentApprovalMode::Unrestricted;
    FString ToolName;
    for (const FString& Candidate : Snapshot->AllowedToolNames)
    {
        if (Candidate.StartsWith(
            TEXT("ue_mcp.editor_toolset.toolsets.blueprint.BlueprintTools.get_graph_dsl_docs_"),
            ESearchCase::CaseSensitive))
        { ToolName = Candidate; break; }
    }
    if (!TestFalse(TEXT("Blueprint tool is available"), ToolName.IsEmpty())) { return false; }
    const TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> Cancelled =
        MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
    FAgentToolCall Call;
    Call.Id = TEXT("unrestricted-ue-tool");
    Call.Name = ToolName;
    Call.ArgumentsJson = TEXT("{}");
    struct FObserved { bool bCompleted = false; bool bFailed = false; };
    const TSharedRef<FObserved> Observed = MakeShared<FObserved>();
    const uint64 StartFrame = GFrameCounter;
    Workflow->Execute({Snapshot, Cancelled, FGuid::NewGuid(), Call}, 10.0,
        [Observed](FMCPToolResult Result)
        {
            Observed->bCompleted = true;
            Observed->bFailed = Result.bIsError;
        });
    TestFalse(TEXT("Even unrestricted UE tools do not dispatch in the caller frame"),
        Observed->bCompleted);
    TestEqual(TEXT("No bridge dispatch occurred in the caller frame"),
        Observer->DispatchFrame, uint64(0));
    const double Deadline = FPlatformTime::Seconds() + 8.0;
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Observer, Observed, StartFrame, Deadline]()
    {
        if (!Observed->bCompleted && FPlatformTime::Seconds() < Deadline) { return false; }
        TestTrue(TEXT("Tool completes"), Observed->bCompleted);
        TestFalse(TEXT("Deferred tool succeeds"), Observed->bFailed);
        TestTrue(TEXT("A normal engine frame elapsed before bridge dispatch"),
            Observer->DispatchFrame > StartFrame);
        return true;
    }));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentToolWorkflowObserverTest,
    "AgentWorkbench.ToolWorkflow.ObserverAfterDispatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentToolWorkflowObserverTest::RunTest(const FString& Parameters)
{
    const TSharedRef<FAgentToolWorkflow> Workflow = MakeShared<FAgentToolWorkflow>();
    const TSharedRef<FCountingObserver> Observer = MakeShared<FCountingObserver>();
    Workflow->AddObserver(Observer);
    const TSharedRef<FAgentRunInputSnapshot> Snapshot = MakeShared<FAgentRunInputSnapshot>();
    FAgentToolBridge::FreezeAvailableTools(*Snapshot);
    Snapshot->bToolListFrozen = true;
    const TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> Cancelled =
        MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
    FAgentToolCall Call;
    Call.Id = TEXT("catalog-call");
    Call.Name = TEXT("ue_mcp.catalog.search");
    Call.ArgumentsJson = TEXT("{\"query\":\"BlueprintTools\",\"limit\":1}");
    bool bCompleted = false;
    Workflow->Execute({Snapshot, Cancelled, FGuid::NewGuid(), Call}, 5.0,
        [&bCompleted, Observer, this](FMCPToolResult Result)
        {
            bCompleted = true;
            TestFalse(TEXT("Catalog dispatch succeeded"), Result.bIsError);
            TestEqual(TEXT("Observer ran before completion"), Observer->AfterCount, 1);
        });
    TestTrue(TEXT("Tool completed"), bCompleted);
    TestEqual(TEXT("Observer entered once"), Observer->BeforeCount, 1);
    TestEqual(TEXT("Observer exited once"), Observer->AfterCount, 1);
    TestEqual(TEXT("Resolved target is passed to observer"), Observer->SeenName,
        FString(TEXT("ue_mcp.catalog.search")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentToolWorkflowCancelBeforeDispatchTest,
    "AgentWorkbench.ToolWorkflow.CancelBeforeDispatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentToolWorkflowCancelBeforeDispatchTest::RunTest(const FString& Parameters)
{
    const TSharedRef<FAgentToolWorkflow> Workflow = MakeShared<FAgentToolWorkflow>();
    const TSharedRef<FDeferredTestGate> Gate = MakeShared<FDeferredTestGate>();
    Workflow->AddGate(Gate);
    const TSharedRef<FAgentRunInputSnapshot> Snapshot = MakeShared<FAgentRunInputSnapshot>();
    Snapshot->bToolListFrozen = true;
    Snapshot->AllowedToolNames.Add(TEXT("bsharness.editor_info"));
    const TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> Cancelled =
        MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
    FAgentToolCall Call;
    Call.Id = TEXT("held-call");
    Call.Name = TEXT("bsharness.editor_info");
    Call.ArgumentsJson = TEXT("{}");
    struct FObserved { int32 Completions = 0; bool bError = false; };
    const TSharedRef<FObserved> Observed = MakeShared<FObserved>();
    Workflow->Execute({Snapshot, Cancelled, FGuid::NewGuid(), Call}, 5.0,
        [Observed](FMCPToolResult Result)
        {
            ++Observed->Completions;
            Observed->bError = Result.bIsError;
        });
    TestTrue(TEXT("Pre-execution gate holds the proposal"), static_cast<bool>(Gate->Pending));
    TestEqual(TEXT("No tool result before gate completes"), Observed->Completions, 0);
    Cancelled->store(true);
    FAgentToolGateContinuation Continue = MoveTemp(Gate->Pending);
    Continue(true, FString());
    const double Deadline = FPlatformTime::Seconds() + 2.0;
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Observed, Deadline]()
    {
        if (Observed->Completions == 0 && FPlatformTime::Seconds() < Deadline) { return false; }
        TestEqual(TEXT("Cancelled proposal completes once"), Observed->Completions, 1);
        TestTrue(TEXT("Cancelled proposal is an error result"), Observed->bError);
        return true;
    }));
    return true;
}
#endif
