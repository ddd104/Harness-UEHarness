#include "AgentToolWorkflow.h"
#include "AgentToolBridge.h"
#include "AgentToolSchemaValidator.h"
#include "AgentUnrealMCPBridge.h"
#include "AgentAssetViewFollow.h"
#include "BSHarnessHookRegistry.h"
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
    double ToolDeadlineSeconds;
    FTSTicker::FDelegateHandle ToolTimeoutHandle;
    TSharedPtr<FJsonObject> Arguments;
    TSharedPtr<FJsonObject> TargetArguments;
    FBSHookToolContext Context;
    FBSHookToolContext FrozenContext;
    FGuid InvocationId;
    FString FrozenTargetName;
    FString FrozenArgumentsJson;
    TFunction<void(FMCPToolResult)> Complete;
    bool bFinished = false; // Game Thread only.
    bool bApproved = false;
    bool bNeedsVisibleEditorFrame = false;
    bool bWasDispatched = false;

    FWorkflowCall(const FAgentToolInvocation& InInvocation, double InTimeoutSeconds,
        TFunction<void(FMCPToolResult)> InComplete)
        : Invocation(InInvocation),
          ToolDeadlineSeconds(FPlatformTime::Seconds() + FMath::Max(0.0, InTimeoutSeconds)),
          Complete(MoveTemp(InComplete))
    {
        Context.SessionId = Invocation.Snapshot->SessionId;
        Context.RunId = Invocation.Snapshot->RunId;
        Context.RequestId = Invocation.RequestId;
        InvocationId = FGuid::NewGuid();
        Context.InvocationId = InvocationId;
        Context.ToolCallId = Invocation.Call.Id;
        Context.RequestedName = Invocation.Call.Name;
        Context.ArgumentsJson = Invocation.Call.ArgumentsJson.IsEmpty()
            ? TEXT("{}") : Invocation.Call.ArgumentsJson;
        switch (Invocation.Snapshot->ApprovalMode)
        {
        case EAgentApprovalMode::Smart:
            Context.ApprovalMode = EBSHookApprovalMode::Smart;
            break;
        case EAgentApprovalMode::Unrestricted:
            Context.ApprovalMode = EBSHookApprovalMode::Unrestricted;
            break;
        default:
            Context.ApprovalMode = EBSHookApprovalMode::Ask;
            break;
        }
        FrozenContext = Context;
    }
};

struct FToolHookRuntime
{
    TArray<FBSHookHandle> Handles;
    FBSHookHandle ApprovalHandle;
    TMap<FGuid, TSharedPtr<FWorkflowCall>> ActiveCalls;
    TMap<FGuid, TSharedPtr<IAgentToolObserverState>> ViewStates;
    TSharedPtr<FAgentAssetViewFollow> ViewFollow;
    FAgentCompilationTracker CompilationTracker;
};

FToolHookRuntime& ToolHookRuntime()
{
    static FToolHookRuntime Runtime;
    return Runtime;
}

TSharedPtr<FWorkflowCall> FindActiveCall(const FGuid& InvocationId)
{
    const TSharedPtr<FWorkflowCall>* Found = ToolHookRuntime().ActiveCalls.Find(InvocationId);
    return Found ? *Found : nullptr;
}

bool ParseArgumentsJson(const FString& Json, TSharedPtr<FJsonObject>& OutArguments)
{
    return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), OutArguments)
        && OutArguments.IsValid();
}

FString SerializeArgumentsJson(const TSharedRef<FJsonObject>& Arguments)
{
    FString Json;
    FJsonSerializer::Serialize(Arguments, TJsonWriterFactory<>::Create(&Json));
    return Json;
}

bool IsUnrealMcpMetaTool(const FString& Name)
{
    return Name == TEXT("ue_mcp.catalog.search")
        || Name == TEXT("ue_mcp.catalog.describe")
        || Name == TEXT("ue_mcp.call_tool");
}

bool ValidateResolvedSchema(const FAgentRunInputSnapshot& Snapshot,
    const FString& TargetName, const TSharedRef<FJsonObject>& Arguments,
    FString& OutError)
{
    // ResolveApprovalTarget already validates frozen UE tool schemas. Native tool
    // schemas also need validation before an approval prompt is presented.
    if (FAgentUnrealMCPBridge::IsUnrealToolName(TargetName)
        && !IsUnrealMcpMetaTool(TargetName)) { return true; }
    const TArray<FMCPToolDefinition> Available = FAgentToolBridge::ListAvailable(Snapshot);
    const FMCPToolDefinition* Definition = Available.FindByPredicate(
        [&TargetName](const FMCPToolDefinition& Candidate)
        { return Candidate.Name.Equals(TargetName, ESearchCase::CaseSensitive); });
    if (!Definition)
    {
        OutError = TEXT("Tool schema is unavailable for the frozen target.");
        return false;
    }
    return FAgentToolSchemaValidator::Validate(
        Definition->InputSchema, Arguments, OutError);
}

bool BuildCompleteApprovalText(const FString& ArgumentsJson,
    FText& OutText, FString& OutReason)
{
    // The approval dialog must show the complete frozen arguments. The event
    // formatter intentionally truncates deep JSON and is unsuitable here.
    if (ArgumentsJson.Len() > 32768)
    {
        OutReason = TEXT("Tool arguments are too large to display completely for approval.");
        return false;
    }
    OutText = FText::FromString(ArgumentsJson);
    return true;
}

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

    FText DisplayArguments;
    if (!BuildCompleteApprovalText(ArgumentsJson, DisplayArguments, OutReason))
    { return false; }

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
                .Text(DisplayArguments)
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

void Finish(const TSharedRef<FWorkflowCall>& Work, EBSHookToolOutcomeKind Kind,
    FMCPToolResult Result)
{
    check(IsInGameThread());
    if (Work->bFinished) { return; }
    Work->bFinished = true;
    if (Work->ToolTimeoutHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(Work->ToolTimeoutHandle);
        Work->ToolTimeoutHandle.Reset();
    }
    FBSHarnessHookRegistry::Get().CancelToolInvocation(Work->InvocationId);
    FBSHookToolOutcome Outcome;
    Outcome.Kind = Kind;
    Outcome.bIsError = Result.bIsError;
    Outcome.ResultJson = SerializeArgumentsJson(Result.ToJson());
    Outcome.bApproved = Work->bApproved;
    Outcome.bWasDispatched = Work->bWasDispatched;
    FBSHarnessHookRegistry::Get().EmitToolAfter(Work->FrozenContext, Outcome);
    ToolHookRuntime().ActiveCalls.Remove(Work->InvocationId);
    TFunction<void(FMCPToolResult)> Complete = MoveTemp(Work->Complete);
    Complete(MoveTemp(Result));
}

bool StopIfCancelledOrTimedOut(const TSharedRef<FWorkflowCall>& Work)
{
    if (Work->bFinished) { return true; }
    if (Work->Invocation.CancellationToken->load())
    {
        Finish(Work, EBSHookToolOutcomeKind::Cancelled,
            FMCPToolResult::Failure(TEXT("Run was cancelled before tool execution.")));
        return true;
    }
    if (Work->Invocation.RunDeadlineSeconds > 0.0
        && FPlatformTime::Seconds() >= Work->Invocation.RunDeadlineSeconds)
    {
        Finish(Work, EBSHookToolOutcomeKind::TimedOut,
            FMCPToolResult::Failure(TEXT("Run exceeded its total timeout before tool execution.")));
        return true;
    }
    if (FPlatformTime::Seconds() >= Work->ToolDeadlineSeconds)
    {
        Finish(Work, EBSHookToolOutcomeKind::TimedOut,
            FMCPToolResult::Failure(TEXT("Tool exceeded its total timeout before execution.")));
        return true;
    }
    return false;
}

void DispatchTool(const TSharedRef<FWorkflowCall>& Work)
{
    check(IsInGameThread());
    if (StopIfCancelledOrTimedOut(Work)) { return; }
    const double ToolRemainingSeconds =
        Work->ToolDeadlineSeconds - FPlatformTime::Seconds();
    const double DispatchSeconds = Work->Invocation.RunDeadlineSeconds > 0.0
        ? FMath::Min(ToolRemainingSeconds,
            Work->Invocation.RunDeadlineSeconds - FPlatformTime::Seconds())
        : ToolRemainingSeconds;
    if (DispatchSeconds <= 0.0)
    {
        Finish(Work, EBSHookToolOutcomeKind::TimedOut,
            FMCPToolResult::Failure(TEXT("Run exceeded its total timeout before tool execution.")));
        return;
    }

    // Re-resolve the exact proposal after all hooks, including the modal approval.
    // Both the resolved target and its serialized arguments must still match the
    // values seen by the approval hook. The bridge repeats its own policy checks.
    FString FinalTarget;
    TSharedPtr<FJsonObject> FinalArguments;
    FString Error;
    if (!FAgentToolBridge::ResolveApprovalTarget(*Work->Invocation.Snapshot,
        Work->Invocation.Call.Name, Work->Arguments.ToSharedRef(),
        FinalTarget, FinalArguments, Error) || !FinalArguments)
    {
        Finish(Work, EBSHookToolOutcomeKind::Invalid,
            FMCPToolResult::ProtocolError(-32602,
                Error.IsEmpty() ? TEXT("Tool target changed before dispatch.") : Error));
        return;
    }
    if (!ValidateResolvedSchema(*Work->Invocation.Snapshot, FinalTarget,
        FinalArguments.ToSharedRef(), Error))
    {
        Finish(Work, EBSHookToolOutcomeKind::Invalid,
            FMCPToolResult::ProtocolError(-32602, Error));
        return;
    }
    if (FinalTarget != Work->FrozenTargetName
        || SerializeArgumentsJson(FinalArguments.ToSharedRef()) != Work->FrozenArgumentsJson
        || Work->Context.SessionId != Work->FrozenContext.SessionId
        || Work->Context.RunId != Work->FrozenContext.RunId
        || Work->Context.RequestId != Work->FrozenContext.RequestId
        || Work->Context.InvocationId != Work->FrozenContext.InvocationId
        || Work->Context.ToolCallId != Work->FrozenContext.ToolCallId
        || Work->Context.RequestedName != Work->FrozenContext.RequestedName
        || Work->Context.TargetName != Work->FrozenContext.TargetName
        || Work->Context.DisplayName != Work->FrozenContext.DisplayName
        || Work->Context.FrozenHandlerId != Work->FrozenContext.FrozenHandlerId
        || Work->Context.ArgumentsJson != Work->FrozenContext.ArgumentsJson
        || Work->Context.bRequiresApproval != Work->FrozenContext.bRequiresApproval
        || Work->Context.ApprovalMode != Work->FrozenContext.ApprovalMode
        || Work->Invocation.Snapshot->AllowedToolHandlerIds.FindRef(FinalTarget)
            != Work->FrozenContext.FrozenHandlerId)
    {
        Finish(Work, EBSHookToolOutcomeKind::Invalid,
            FMCPToolResult::ProtocolError(-32602,
                TEXT("Tool target or arguments changed after hook review.")));
        return;
    }
    if (!Work->bApproved
        && FAgentToolBridge::RequiresApproval(*Work->Invocation.Snapshot,
            Work->Invocation.Call.Name))
    {
        Finish(Work, EBSHookToolOutcomeKind::Denied,
            FMCPToolResult::Failure(TEXT("Tool now requires approval before dispatch.")));
        return;
    }
    Work->bWasDispatched = true;
    FAgentToolBridge::Execute(*Work->Invocation.Snapshot, Work->Invocation.Call.Name,
        Work->Arguments.ToSharedRef(), DispatchSeconds, Work->bApproved,
        [Work](FMCPToolResult Result)
        {
            Finish(Work, EBSHookToolOutcomeKind::Dispatched, MoveTemp(Result));
        });
}

void ReadyAndSchedule(const TSharedRef<FWorkflowCall>& Work)
{
    check(IsInGameThread());
    if (StopIfCancelledOrTimedOut(Work)) { return; }
    const bool bReadyNeedsFrame =
        FBSHarnessHookRegistry::Get().EmitToolReady(Work->Context);
    Work->bNeedsVisibleEditorFrame |= bReadyNeedsFrame;
    if (StopIfCancelledOrTimedOut(Work)) { return; }

    // UE toolsets can synchronously mutate or reinstance UObjects. A modal approval
    // window runs its own Slate loop with the engine tick paused, so dispatching here
    // would run inside the suspended outer frame. Yield through the core ticker even
    // in Unrestricted mode, where no approval window is shown.
    const bool bDeferUntilEngineFrame = Work->bNeedsVisibleEditorFrame
        || (FAgentUnrealMCPBridge::IsUnrealToolName(Work->Context.TargetName)
            && !IsUnrealMcpMetaTool(Work->Context.TargetName));
    if (bDeferUntilEngineFrame)
    {
        // The first core-ticker pass ends the current engine frame. The second runs
        // after the next GEngine->Tick, outside the modal window's nested Slate loop.
        // This also gives newly opened asset editors a chance to paint.
        FTSTicker::GetCoreTicker().AddTicker(TEXT("AgentWorkbenchDeferredTool"), 0.0f,
            [Work, FramesToWait = 2](float) mutable
            {
                if (--FramesToWait > 0) { return true; }
                DispatchTool(Work);
                return false;
            });
    }
    else { DispatchTool(Work); }
}

void RunBeforeAndSchedule(const TSharedRef<FWorkflowCall>& Work)
{
    check(IsInGameThread());
    if (StopIfCancelledOrTimedOut(Work)) { return; }
    FBSHarnessHookRegistry::Get().RunToolBefore(Work->Context,
        [Work](FBSHookToolBeforeResult Before)
        {
            if (StopIfCancelledOrTimedOut(Work)) { return; }
            switch (Before.Decision)
            {
            case EBSHookToolDecision::Deny:
                Finish(Work, EBSHookToolOutcomeKind::Denied,
                    FMCPToolResult::Failure(Before.Reason.IsEmpty()
                        ? TEXT("Tool call was rejected before execution.") : Before.Reason));
                return;
            case EBSHookToolDecision::Handled:
                Finish(Work, EBSHookToolOutcomeKind::Handled, MoveTemp(Before.Result));
                return;
            default:
                Work->bNeedsVisibleEditorFrame = Before.bNeedsVisibleEditorFrame;
                ReadyAndSchedule(Work);
                return;
            }
        });
}

void RequestApprovalOrRunBefore(const TSharedRef<FWorkflowCall>& Work)
{
    check(IsInGameThread());
    if (StopIfCancelledOrTimedOut(Work)) { return; }
    if (!Work->Context.bRequiresApproval)
    {
        RunBeforeAndSchedule(Work);
        return;
    }
    if (!ToolHookRuntime().ApprovalHandle.IsValid()
        || !FBSHarnessHookRegistry::Get().IsRegistered(
            ToolHookRuntime().ApprovalHandle))
    {
        Finish(Work, EBSHookToolOutcomeKind::Denied,
            FMCPToolResult::Failure(TEXT("Required tool approval hook is unavailable.")));
        return;
    }
    FBSHarnessHookRegistry::Get().RequestToolApproval(Work->Context,
        [Work](FBSHookToolApprovalResult Approval)
        {
            if (StopIfCancelledOrTimedOut(Work)) { return; }
            if (!Approval.bApproved)
            {
                Finish(Work, EBSHookToolOutcomeKind::Denied,
                    FMCPToolResult::Failure(Approval.Reason.IsEmpty()
                        ? TEXT("Tool call was not approved.") : Approval.Reason));
                return;
            }
            Work->bApproved = true;
            RunBeforeAndSchedule(Work);
        });
}

void StartTool(const TSharedRef<FWorkflowCall>& Work)
{
    check(IsInGameThread());
    if (StopIfCancelledOrTimedOut(Work)) { return; }
    if (!ParseArgumentsJson(Work->Context.ArgumentsJson, Work->Arguments))
    {
        Finish(Work, EBSHookToolOutcomeKind::Invalid,
            FMCPToolResult::ProtocolError(-32602,
                TEXT("Model tool arguments must be a JSON object.")));
        return;
    }
    FString TargetName;
    TSharedPtr<FJsonObject> TargetArguments;
    FString TargetError;
    if (!FAgentToolBridge::ResolveApprovalTarget(*Work->Invocation.Snapshot,
        Work->Invocation.Call.Name, Work->Arguments.ToSharedRef(),
        TargetName, TargetArguments, TargetError) || !TargetArguments)
    {
        Finish(Work, EBSHookToolOutcomeKind::Invalid,
            FMCPToolResult::ProtocolError(-32602,
                TargetError.IsEmpty() ? TEXT("Tool target is invalid.") : TargetError));
        return;
    }
    if (!ValidateResolvedSchema(*Work->Invocation.Snapshot, TargetName,
        TargetArguments.ToSharedRef(), TargetError))
    {
        Finish(Work, EBSHookToolOutcomeKind::Invalid,
            FMCPToolResult::ProtocolError(-32602, TargetError));
        return;
    }
    Work->TargetArguments = TargetArguments;
    Work->FrozenTargetName = TargetName;
    Work->FrozenArgumentsJson = SerializeArgumentsJson(TargetArguments.ToSharedRef());
    Work->Context.TargetName = TargetName;
    Work->Context.FrozenHandlerId =
        Work->Invocation.Snapshot->AllowedToolHandlerIds.FindRef(TargetName);
    Work->Context.DisplayName =
        FAgentToolBridge::GetDisplayName(*Work->Invocation.Snapshot, TargetName);
    Work->Context.ArgumentsJson = Work->FrozenArgumentsJson;
    Work->Context.bRequiresApproval =
        FAgentToolBridge::RequiresApproval(*Work->Invocation.Snapshot,
            Work->Invocation.Call.Name);
    Work->FrozenContext = Work->Context;
    // A before hook can short-circuit a call, so approval must happen first.
    RequestApprovalOrRunBefore(Work);
}
}

void RegisterAgentWorkbenchToolHooks()
{
    check(IsInGameThread());
    FToolHookRuntime& Runtime = ToolHookRuntime();
    if (!Runtime.Handles.IsEmpty()) { return; }
    Runtime.ViewFollow = MakeShared<FAgentAssetViewFollow>();
    FBSHarnessHookRegistry& Registry = FBSHarnessHookRegistry::Get();

    Runtime.Handles.Add(Registry.RegisterToolBefore(
        TEXT("AgentWorkbench.BlueprintCompileDedup"), 0,
        [](const FBSHookToolContext& Context, FBSHookToolBeforeContinuation Continue)
        {
            FBSHookToolBeforeResult Decision;
            TSharedPtr<FJsonObject> Arguments;
            if (ParseArgumentsJson(Context.ArgumentsJson, Arguments)
                && IsRedundantBlueprintCompile(ToolHookRuntime().CompilationTracker,
                    Context.RunId, Context.DisplayName, *Arguments))
            {
                Decision.Decision = EBSHookToolDecision::Handled;
                Decision.Result = FMCPToolResult::Success(
                    TEXT("Blueprint was already compiled by the successful write_graph_dsl call in this run. No second compilation was performed."));
            }
            Continue(MoveTemp(Decision));
        }));
    Runtime.ApprovalHandle = Registry.RegisterToolApproval(
        TEXT("AgentWorkbench.ToolApproval"), 0,
        [](const FBSHookToolContext& Context, FBSHookToolApprovalContinuation Continue)
        {
            FBSHookToolApprovalResult Decision;
            const EAgentApprovalMode Mode = Context.ApprovalMode == EBSHookApprovalMode::Smart
                ? EAgentApprovalMode::Smart
                : Context.ApprovalMode == EBSHookApprovalMode::Unrestricted
                    ? EAgentApprovalMode::Unrestricted : EAgentApprovalMode::Ask;
            Decision.bApproved =
                ShouldAutomaticallyApprove(Mode, Context.TargetName, Context.DisplayName);
            if (!Decision.bApproved)
            {
                Decision.bApproved = ConfirmUnrealMCPTool(
                    Context.DisplayName, Context.ArgumentsJson, Decision.Reason);
            }
            Continue(MoveTemp(Decision));
        });
    Runtime.Handles.Add(Runtime.ApprovalHandle);
    Runtime.Handles.Add(Registry.RegisterToolReady(
        TEXT("AgentWorkbench.AssetViewFollow"), 0,
        [](const FBSHookToolContext& Context)
        {
            FToolHookRuntime& Runtime = ToolHookRuntime();
            const TSharedPtr<FWorkflowCall> Work = FindActiveCall(Context.InvocationId);
            if (!Work || !Work->TargetArguments || !Runtime.ViewFollow) { return false; }
            const TSharedRef<const FJsonObject> Arguments = Work->TargetArguments.ToSharedRef();
            const FAgentResolvedToolInvocation Resolved{Work->Invocation, Context.TargetName,
                Context.DisplayName, Arguments};
            TSharedPtr<IAgentToolObserverState> State =
                Runtime.ViewFollow->BeforeExecute(Resolved);
            if (!State) { return false; }
            const bool bNeedsFrame = State->NeedsVisibleEditorFrame();
            Runtime.ViewStates.Add(Context.InvocationId, MoveTemp(State));
            return bNeedsFrame;
        }));
    Runtime.Handles.Add(Registry.RegisterToolAfter(
        TEXT("AgentWorkbench.BlueprintCompileTracker"), 0,
        [](const FBSHookToolContext& Context, const FBSHookToolOutcome& Outcome)
        {
            if (Outcome.Kind != EBSHookToolOutcomeKind::Dispatched
                || !Outcome.bWasDispatched) { return; }
            TSharedPtr<FJsonObject> Arguments;
            if (!ParseArgumentsJson(Context.ArgumentsJson, Arguments)) { return; }
            FMCPToolResult Result;
            Result.bIsError = Outcome.bIsError;
            RecordBlueprintCompilation(ToolHookRuntime().CompilationTracker,
                Context.RunId, Context.DisplayName, *Arguments, Result);
        }));
    Runtime.Handles.Add(Registry.RegisterToolAfter(
        TEXT("AgentWorkbench.AssetViewFollow"), 100,
        [](const FBSHookToolContext& Context, const FBSHookToolOutcome& Outcome)
        {
            FToolHookRuntime& Runtime = ToolHookRuntime();
            TSharedPtr<IAgentToolObserverState> State;
            if (TSharedPtr<IAgentToolObserverState>* Found =
                Runtime.ViewStates.Find(Context.InvocationId))
            { State = *Found; }
            Runtime.ViewStates.Remove(Context.InvocationId);
            if (!State || !Runtime.ViewFollow || !Outcome.bWasDispatched
                || Outcome.Kind != EBSHookToolOutcomeKind::Dispatched) { return; }
            const TSharedPtr<FWorkflowCall> Work = FindActiveCall(Context.InvocationId);
            if (!Work || !Work->TargetArguments) { return; }
            const TSharedRef<const FJsonObject> Arguments = Work->TargetArguments.ToSharedRef();
            const FAgentResolvedToolInvocation Resolved{Work->Invocation, Context.TargetName,
                Context.DisplayName, Arguments};
            FMCPToolResult Result;
            Result.bIsError = Outcome.bIsError;
            Runtime.ViewFollow->AfterExecute(Resolved, State, Result);
        }));
    Runtime.Handles.Add(Registry.RegisterRunExited(
        TEXT("AgentWorkbench.ToolRunCleanup"), -1000000,
        [](const FBSHookRunExitContext& Context)
        {
            FToolHookRuntime& Runtime = ToolHookRuntime();
            TArray<TSharedRef<FWorkflowCall>> Pending;
            for (const TPair<FGuid, TSharedPtr<FWorkflowCall>>& Entry : Runtime.ActiveCalls)
            {
                if (Entry.Value && Entry.Value->Invocation.Snapshot->RunId == Context.RunId)
                { Pending.Add(Entry.Value.ToSharedRef()); }
            }
            const bool bTimedOut = Context.Reason == EBSHookRunExitReason::TimedOut;
            for (const TSharedRef<FWorkflowCall>& Work : Pending)
            {
                Finish(Work, bTimedOut ? EBSHookToolOutcomeKind::TimedOut
                    : EBSHookToolOutcomeKind::Cancelled,
                    FMCPToolResult::Failure(Context.Detail.IsEmpty()
                        ? TEXT("Run exited before tool execution completed.")
                        : Context.Detail));
            }
            Runtime.CompilationTracker.AutomaticallyCompiledBlueprints.Remove(Context.RunId);
        }));
    for (const FBSHookHandle& Handle : Runtime.Handles)
    {
        ensureMsgf(Handle.IsValid(), TEXT("Failed to register an AgentWorkbench tool hook."));
    }
}

void UnregisterAgentWorkbenchToolHooks()
{
    check(IsInGameThread());
    FToolHookRuntime& Runtime = ToolHookRuntime();
    TArray<TSharedRef<FWorkflowCall>> Pending;
    for (const TPair<FGuid, TSharedPtr<FWorkflowCall>>& Entry : Runtime.ActiveCalls)
    {
        if (Entry.Value) { Pending.Add(Entry.Value.ToSharedRef()); }
    }
    for (const TSharedRef<FWorkflowCall>& Work : Pending)
    {
        Finish(Work, EBSHookToolOutcomeKind::Cancelled,
            FMCPToolResult::Failure(TEXT("Workbench tool hooks are shutting down.")));
    }
    FBSHarnessHookRegistry& Registry = FBSHarnessHookRegistry::Get();
    for (const FBSHookHandle& Handle : Runtime.Handles)
    {
        if (Handle.IsValid()) { Registry.Unregister(Handle); }
    }
    Runtime.Handles.Reset();
    Runtime.ApprovalHandle = FBSHookHandle();
    Runtime.ActiveCalls.Reset();
    Runtime.ViewStates.Reset();
    Runtime.ViewFollow.Reset();
    Runtime.CompilationTracker.AutomaticallyCompiledBlueprints.Reset();
}

void FAgentToolWorkflow::Execute(const FAgentToolInvocation& Invocation, double TimeoutSeconds,
    TFunction<void(FMCPToolResult)> Complete) const
{
    check(IsInGameThread());
    const TSharedRef<FWorkflowCall> Work =
        MakeShared<FWorkflowCall>(Invocation, TimeoutSeconds, MoveTemp(Complete));
    ToolHookRuntime().ActiveCalls.Add(Work->InvocationId, Work);
    Work->ToolTimeoutHandle = FTSTicker::GetCoreTicker().AddTicker(
        TEXT("AgentWorkbenchToolTimeout"), 0.05f,
        [WeakWork = TWeakPtr<FWorkflowCall>(Work)](float)
        {
            const TSharedPtr<FWorkflowCall> Pending = WeakWork.Pin();
            if (!Pending || Pending->bFinished) { return false; }
            if (StopIfCancelledOrTimedOut(Pending.ToSharedRef())) { return false; }
            return true;
        });
    StartTool(Work);
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
    FString DeepArguments = TEXT("0");
    for (int32 Depth = 0; Depth < 48; ++Depth)
    { DeepArguments = FString::Printf(TEXT("{\"nested\":%s}"), *DeepArguments); }
    FText ApprovalText;
    FString ApprovalError;
    TestTrue(TEXT("Deep arguments fit the complete approval display"),
        BuildCompleteApprovalText(DeepArguments, ApprovalText, ApprovalError));
    TestEqual(TEXT("Approval display preserves every nested argument"),
        ApprovalText.ToString(), DeepArguments);
    TestFalse(TEXT("Oversized arguments cannot be approved from a shortened view"),
        BuildCompleteApprovalText(FString::ChrN(32769, TEXT('x')),
            ApprovalText, ApprovalError));
    return true;
}

namespace
{
struct FHookTestObservation
{
    int32 BeforeCount = 0;
    int32 AfterCount = 0;
    FString SeenName;
    bool bCompleted = false;
    bool bFailed = false;
    uint64 CompletionFrame = 0;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentToolWorkflowVisibleEditorFrameTest,
    "AgentWorkbench.ToolWorkflow.VisibleEditorBeforeDispatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentToolWorkflowVisibleEditorFrameTest::RunTest(const FString& Parameters)
{
    FBSHarnessHookRegistry& Hooks = FBSHarnessHookRegistry::Get();
    const TSharedRef<FHookTestObservation> Observed = MakeShared<FHookTestObservation>();
    const FBSHookHandle Ready = Hooks.RegisterToolReady(
        TEXT("AgentWorkbench.Test.VisibleFrame"), -100,
        [Observed](const FBSHookToolContext& Context)
        {
            if (Context.ToolCallId != TEXT("visible-frame-call")) { return false; }
            ++Observed->BeforeCount;
            return true;
        });
    const FBSHookHandle After = Hooks.RegisterToolAfter(
        TEXT("AgentWorkbench.Test.VisibleFrame"), -100,
        [Observed](const FBSHookToolContext& Context, const FBSHookToolOutcome&)
        {
            if (Context.ToolCallId == TEXT("visible-frame-call")) { ++Observed->AfterCount; }
        });
    if (!TestTrue(TEXT("Test hooks registered"), Ready.IsValid() && After.IsValid()))
    {
        Hooks.Unregister(Ready);
        Hooks.Unregister(After);
        return false;
    }
    const TSharedRef<FAgentToolWorkflow> Workflow = MakeShared<FAgentToolWorkflow>();
    const TSharedRef<FAgentRunInputSnapshot> Snapshot = MakeShared<FAgentRunInputSnapshot>();
    FAgentToolBridge::FreezeAvailableTools(*Snapshot);
    Snapshot->bToolListFrozen = true;
    const TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> Cancelled =
        MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
    FAgentToolCall Call;
    Call.Id = TEXT("visible-frame-call");
    Call.Name = TEXT("ue_mcp.catalog.search");
    Call.ArgumentsJson = TEXT("{\"query\":\"BlueprintTools\",\"limit\":1}");
    Workflow->Execute({Snapshot, Cancelled, FGuid::NewGuid(), Call}, 5.0,
        [Observed](FMCPToolResult Result)
        {
            Observed->bCompleted = true;
            Observed->bFailed = Result.bIsError;
        });
    TestEqual(TEXT("Ready hook prepared view before dispatch"), Observed->BeforeCount, 1);
    TestFalse(TEXT("Dispatch waits for an editor frame"), Observed->bCompleted);
    const double Deadline = FPlatformTime::Seconds() + 3.0;
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand(
        [this, Observed, Ready, After, Deadline]()
    {
        if (!Observed->bCompleted && FPlatformTime::Seconds() < Deadline) { return false; }
        TestTrue(TEXT("Tool completes after the frame"), Observed->bCompleted);
        TestFalse(TEXT("Deferred tool succeeds"), Observed->bFailed);
        TestEqual(TEXT("Post-execution hook runs"), Observed->AfterCount, 1);
        FBSHarnessHookRegistry::Get().Unregister(Ready);
        FBSHarnessHookRegistry::Get().Unregister(After);
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
    const TSharedRef<FHookTestObservation> Observed = MakeShared<FHookTestObservation>();
    const uint64 StartFrame = GFrameCounter;
    Workflow->Execute({Snapshot, Cancelled, FGuid::NewGuid(), Call}, 10.0,
        [Observed](FMCPToolResult Result)
        {
            Observed->bCompleted = true;
            Observed->bFailed = Result.bIsError;
            Observed->CompletionFrame = GFrameCounter;
        });
    TestFalse(TEXT("Even unrestricted UE tools do not dispatch in the caller frame"),
        Observed->bCompleted);
    const double Deadline = FPlatformTime::Seconds() + 8.0;
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand(
        [this, Observed, StartFrame, Deadline]()
    {
        if (!Observed->bCompleted && FPlatformTime::Seconds() < Deadline) { return false; }
        TestTrue(TEXT("Tool completes"), Observed->bCompleted);
        TestFalse(TEXT("Deferred tool succeeds"), Observed->bFailed);
        TestTrue(TEXT("A normal engine frame elapsed before completion"),
            Observed->CompletionFrame > StartFrame);
        return true;
    }));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentToolWorkflowObserverTest,
    "AgentWorkbench.ToolWorkflow.ObserverAfterDispatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentToolWorkflowObserverTest::RunTest(const FString& Parameters)
{
    FBSHarnessHookRegistry& Hooks = FBSHarnessHookRegistry::Get();
    const TSharedRef<FHookTestObservation> Observed = MakeShared<FHookTestObservation>();
    const FBSHookHandle Ready = Hooks.RegisterToolReady(
        TEXT("AgentWorkbench.Test.ToolReady"), -100,
        [Observed](const FBSHookToolContext& Context)
        {
            if (Context.ToolCallId != TEXT("catalog-call")) { return false; }
            ++Observed->BeforeCount;
            Observed->SeenName = Context.TargetName;
            return false;
        });
    const FBSHookHandle After = Hooks.RegisterToolAfter(
        TEXT("AgentWorkbench.Test.ToolAfter"), -100,
        [Observed](const FBSHookToolContext& Context, const FBSHookToolOutcome&)
        {
            if (Context.ToolCallId != TEXT("catalog-call")) { return; }
            ++Observed->AfterCount;
            Observed->SeenName = Context.TargetName;
        });
    if (!TestTrue(TEXT("Test hooks registered"), Ready.IsValid() && After.IsValid()))
    {
        Hooks.Unregister(Ready);
        Hooks.Unregister(After);
        return false;
    }
    const TSharedRef<FAgentToolWorkflow> Workflow = MakeShared<FAgentToolWorkflow>();
    const TSharedRef<FAgentRunInputSnapshot> Snapshot = MakeShared<FAgentRunInputSnapshot>();
    FAgentToolBridge::FreezeAvailableTools(*Snapshot);
    Snapshot->bToolListFrozen = true;
    const TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> Cancelled =
        MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
    FAgentToolCall Call;
    Call.Id = TEXT("catalog-call");
    Call.Name = TEXT("ue_mcp.catalog.search");
    Call.ArgumentsJson = TEXT("{\"query\":\"BlueprintTools\",\"limit\":1}");
    Workflow->Execute({Snapshot, Cancelled, FGuid::NewGuid(), Call}, 5.0,
        [Observed, this](FMCPToolResult Result)
        {
            Observed->bCompleted = true;
            TestFalse(TEXT("Catalog dispatch succeeded"), Result.bIsError);
            TestEqual(TEXT("After hook ran before completion"), Observed->AfterCount, 1);
        });
    TestTrue(TEXT("Tool completed synchronously"), Observed->bCompleted);
    TestEqual(TEXT("Ready hook entered once"), Observed->BeforeCount, 1);
    TestEqual(TEXT("After hook exited once"), Observed->AfterCount, 1);
    TestEqual(TEXT("Resolved target is passed to hook"), Observed->SeenName,
        FString(TEXT("ue_mcp.catalog.search")));
    Hooks.Unregister(Ready);
    Hooks.Unregister(After);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentToolWorkflowCancelBeforeDispatchTest,
    "AgentWorkbench.ToolWorkflow.CancelBeforeDispatch",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentToolWorkflowCancelBeforeDispatchTest::RunTest(const FString& Parameters)
{
    struct FPendingState
    {
        FBSHookToolBeforeContinuation Continue;
        int32 Completions = 0;
        bool bError = false;
    };
    const TSharedRef<FPendingState> Pending = MakeShared<FPendingState>();
    FBSHarnessHookRegistry& Hooks = FBSHarnessHookRegistry::Get();
    const FBSHookHandle Before = Hooks.RegisterToolBefore(
        TEXT("AgentWorkbench.Test.DeferredBefore"), -100,
        [Pending](const FBSHookToolContext& Context,
            FBSHookToolBeforeContinuation Continue)
        {
            if (Context.ToolCallId == TEXT("held-call"))
            { Pending->Continue = MoveTemp(Continue); }
            else { Continue(FBSHookToolBeforeResult()); }
        });
    if (!TestTrue(TEXT("Deferred hook registered"), Before.IsValid())) { return false; }
    const TSharedRef<FAgentToolWorkflow> Workflow = MakeShared<FAgentToolWorkflow>();
    const TSharedRef<FAgentRunInputSnapshot> Snapshot = MakeShared<FAgentRunInputSnapshot>();
    FAgentToolBridge::FreezeAvailableTools(*Snapshot);
    Snapshot->bToolListFrozen = true;
    const TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> Cancelled =
        MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
    FAgentToolCall Call;
    Call.Id = TEXT("held-call");
    Call.Name = TEXT("bsharness.editor_info");
    Call.ArgumentsJson = TEXT("{}");
    Workflow->Execute({Snapshot, Cancelled, FGuid::NewGuid(), Call}, 5.0,
        [Pending](FMCPToolResult Result)
        {
            ++Pending->Completions;
            Pending->bError = Result.bIsError;
        });
    TestTrue(TEXT("Before hook holds the proposal"), static_cast<bool>(Pending->Continue));
    TestEqual(TEXT("No tool result before hook completes"), Pending->Completions, 0);
    Cancelled->store(true);
    FBSHookToolBeforeContinuation Continue = MoveTemp(Pending->Continue);
    if (Continue) { Continue(FBSHookToolBeforeResult()); }
    const double Deadline = FPlatformTime::Seconds() + 2.0;
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand(
        [this, Pending, Before, Deadline]()
    {
        if (Pending->Completions == 0 && FPlatformTime::Seconds() < Deadline) { return false; }
        TestEqual(TEXT("Cancelled proposal completes once"), Pending->Completions, 1);
        TestTrue(TEXT("Cancelled proposal is an error result"), Pending->bError);
        FBSHarnessHookRegistry::Get().Unregister(Before);
        return true;
    }));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentToolWorkflowHookTimeoutTest,
    "AgentWorkbench.ToolWorkflow.HookTimeoutClosesOnce",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentToolWorkflowHookTimeoutTest::RunTest(const FString& Parameters)
{
    struct FPendingState
    {
        FBSHookToolBeforeContinuation Continue;
        int32 AfterCount = 0;
        int32 CompletionCount = 0;
        bool bWasDispatched = false;
        EBSHookToolOutcomeKind Kind = EBSHookToolOutcomeKind::Invalid;
    };
    const TSharedRef<FPendingState> Pending = MakeShared<FPendingState>();
    FBSHarnessHookRegistry& Hooks = FBSHarnessHookRegistry::Get();
    const FBSHookHandle Before = Hooks.RegisterToolBefore(
        TEXT("AgentWorkbench.Test.HeldUntilTimeout"), -100,
        [Pending](const FBSHookToolContext& Context,
            FBSHookToolBeforeContinuation Continue)
        {
            if (Context.ToolCallId == TEXT("timeout-held-call"))
            { Pending->Continue = MoveTemp(Continue); }
            else { Continue(FBSHookToolBeforeResult()); }
        });
    const FBSHookHandle After = Hooks.RegisterToolAfter(
        TEXT("AgentWorkbench.Test.HeldUntilTimeout"), -100,
        [Pending](const FBSHookToolContext& Context,
            const FBSHookToolOutcome& Outcome)
        {
            if (Context.ToolCallId != TEXT("timeout-held-call")) { return; }
            ++Pending->AfterCount;
            Pending->Kind = Outcome.Kind;
            Pending->bWasDispatched = Outcome.bWasDispatched;
        });
    if (!TestTrue(TEXT("Timeout test hooks registered"),
        Before.IsValid() && After.IsValid()))
    {
        Hooks.Unregister(Before);
        Hooks.Unregister(After);
        return false;
    }
    const TSharedRef<FAgentToolWorkflow> Workflow = MakeShared<FAgentToolWorkflow>();
    const TSharedRef<FAgentRunInputSnapshot> Snapshot = MakeShared<FAgentRunInputSnapshot>();
    FAgentToolBridge::FreezeAvailableTools(*Snapshot);
    Snapshot->bToolListFrozen = true;
    const TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> Cancelled =
        MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
    FAgentToolCall Call;
    Call.Id = TEXT("timeout-held-call");
    Call.Name = TEXT("bsharness.editor_info");
    Call.ArgumentsJson = TEXT("{}");
    Workflow->Execute({Snapshot, Cancelled, FGuid::NewGuid(), Call}, 0.05,
        [Pending](FMCPToolResult) { ++Pending->CompletionCount; });
    TestTrue(TEXT("Before hook retained its continuation"),
        static_cast<bool>(Pending->Continue));
    TestEqual(TEXT("No premature completion"), Pending->CompletionCount, 0);
    const double Deadline = FPlatformTime::Seconds() + 3.0;
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand(
        [this, Pending, Before, After, Deadline]()
    {
        if (Pending->CompletionCount == 0 && FPlatformTime::Seconds() < Deadline)
        { return false; }
        TestEqual(TEXT("Timed out tool completes once"), Pending->CompletionCount, 1);
        TestEqual(TEXT("Timed out tool emits one After event"), Pending->AfterCount, 1);
        TestTrue(TEXT("Timeout outcome is reported"),
            Pending->Kind == EBSHookToolOutcomeKind::TimedOut);
        TestFalse(TEXT("Held tool was never dispatched"), Pending->bWasDispatched);
        FBSHookToolBeforeContinuation Late = MoveTemp(Pending->Continue);
        if (Late) { Late(FBSHookToolBeforeResult()); }
        TestEqual(TEXT("Late continuation cannot complete again"),
            Pending->CompletionCount, 1);
        TestEqual(TEXT("Late continuation cannot emit another After event"),
            Pending->AfterCount, 1);
        FBSHarnessHookRegistry::Get().Unregister(Before);
        FBSHarnessHookRegistry::Get().Unregister(After);
        return true;
    }));
    return true;
}
#endif
