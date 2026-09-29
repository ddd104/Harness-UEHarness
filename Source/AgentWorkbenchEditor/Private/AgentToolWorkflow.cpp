#include "AgentToolWorkflow.h"
#include "AgentToolBridge.h"
#include "AgentWorkbenchDisplayFormatter.h"
#include "Async/Async.h"
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
#include <atomic>

namespace
{
struct FWorkflowCall
{
    FAgentToolInvocation Invocation;
    TArray<TSharedRef<IAgentToolGate>> Gates;
    double ToolTimeoutSeconds;
    TSharedPtr<FJsonObject> Arguments;
    TFunction<void(FMCPToolResult)> Complete;
    bool bFinished = false; // Game Thread only.

    FWorkflowCall(const FAgentToolInvocation& InInvocation,
        const TArray<TSharedRef<IAgentToolGate>>& InGates, double InTimeoutSeconds,
        TFunction<void(FMCPToolResult)> InComplete)
        : Invocation(InInvocation), Gates(InGates),
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
        const bool bRequiresApproval = FAgentToolBridge::RequiresApproval(
            *Work->Invocation.Snapshot, Work->Invocation.Call.Name);
        bool bApproved = false;
        if (bRequiresApproval)
        {
            FString TargetName;
            TSharedPtr<FJsonObject> TargetArguments;
            FString Error;
            if (!FAgentToolBridge::ResolveApprovalTarget(*Work->Invocation.Snapshot,
                Work->Invocation.Call.Name, Work->Arguments.ToSharedRef(),
                TargetName, TargetArguments, Error) || !TargetArguments)
            {
                Finish(Work, FMCPToolResult::ProtocolError(-32602,
                    Error.IsEmpty() ? TEXT("Tool approval target is invalid.") : Error));
                return;
            }
            FString TargetArgumentsJson;
            FJsonSerializer::Serialize(TargetArguments.ToSharedRef(),
                TJsonWriterFactory<>::Create(&TargetArgumentsJson));
            bApproved = ConfirmUnrealMCPTool(
                FAgentToolBridge::GetDisplayName(*Work->Invocation.Snapshot, TargetName),
                TargetArgumentsJson, Error);
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
        FAgentToolBridge::Execute(*Work->Invocation.Snapshot, Work->Invocation.Call.Name,
            Work->Arguments.ToSharedRef(), RemainingSeconds, bApproved,
            [Work](FMCPToolResult Result) { Finish(Work, MoveTemp(Result)); });
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

void FAgentToolWorkflow::AddGate(const TSharedRef<IAgentToolGate>& Gate)
{
    check(IsInGameThread());
    Gates.Add(Gate);
}

void FAgentToolWorkflow::Execute(const FAgentToolInvocation& Invocation, double TimeoutSeconds,
    TFunction<void(FMCPToolResult)> Complete) const
{
    check(IsInGameThread());
    const TSharedRef<FWorkflowCall> Work = MakeShared<FWorkflowCall>(Invocation, Gates,
        TimeoutSeconds, MoveTemp(Complete));
    Advance(Work, 0);
}

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
class FDeferredTestGate final : public IAgentToolGate
{
public:
    FAgentToolGateContinuation Pending;
    virtual void Evaluate(const FAgentToolInvocation&, FAgentToolGateContinuation Continue) override
    { Pending = MoveTemp(Continue); }
};
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
