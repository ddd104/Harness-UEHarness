#include "BSHarnessHookRegistry.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/Async.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBSHarnessHookRegistryOrderingTest,
    "BSHarness.Hooks.Registry.OrderingAndLifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBSHarnessHookRegistryOrderingTest::RunTest(const FString& Parameters)
{
    FBSHarnessHookRegistry& Registry = FBSHarnessHookRegistry::Get();
    TArray<FBSHookHandle> Handles;
    TArray<FString> Order;

    Handles.Add(Registry.RegisterPromptSubmitting(TEXT("Test.Hooks.PromptLate"), 20,
        [&Order](FBSHookPromptContext& Context, FString&)
        {
            Order.Add(TEXT("late"));
            Context.AdditionalModelContext += TEXT("extra");
            return true;
        }));
    Handles.Add(Registry.RegisterPromptSubmitting(TEXT("Test.Hooks.PromptEarly"), -10,
        [&Order](FBSHookPromptContext&, FString&)
        {
            Order.Add(TEXT("early"));
            return true;
        }));
    int32 SubmittedCount = 0;
    Handles.Add(Registry.RegisterPromptSubmitted(TEXT("Test.Hooks.Submitted"), 0,
        [&SubmittedCount](const FBSHookPromptContext& Context)
        {
            if (Context.AdditionalModelContext == TEXT("extra")) { ++SubmittedCount; }
        }));

    FBSHookPromptContext Prompt;
    Prompt.SessionId = FGuid::NewGuid();
    Prompt.RunId = FGuid::NewGuid();
    Prompt.UserPrompt = TEXT("original");
    Prompt.ModelId = TEXT("test-model");
    FString Reason;
    TestTrue(TEXT("Prompt proceeds"), Registry.EmitPromptSubmitting(Prompt, Reason));
    TestEqual(TEXT("Stable priority order"), FString::Join(Order, TEXT(",")), TEXT("early,late"));
    TestEqual(TEXT("Only model context is appended"), Prompt.AdditionalModelContext, TEXT("extra"));
    Registry.EmitPromptSubmitted(Prompt);
    TestEqual(TEXT("Submitted notification"), SubmittedCount, 1);

    const FBSHookHandle Duplicate = Registry.RegisterPromptSubmitted(TEXT("Test.Hooks.Submitted"), 0,
        [](const FBSHookPromptContext&) {});
    TestFalse(TEXT("Duplicate point/name is rejected"), Duplicate.IsValid());
    const FBSHookHandle CompetingApproval = Registry.RegisterToolApproval(
        TEXT("Test.Hooks.CompetingApproval"), 0,
        [](const FBSHookToolContext&, FBSHookToolApprovalContinuation Continue)
        {
            FBSHookToolApprovalResult Result;
            Result.bApproved = true;
            Continue(MoveTemp(Result));
        });
    TestFalse(TEXT("Workbench owns the exclusive approval slot"), CompetingApproval.IsValid());
    if (CompetingApproval.IsValid())
    {
        Registry.Unregister(CompetingApproval);
    }

    Handles.Add(Registry.RegisterPromptSubmitting(TEXT("Test.Hooks.Veto"), 30,
        [](FBSHookPromptContext&, FString& OutReason)
        {
            OutReason = TEXT("vetoed");
            return false;
        }));
    TestFalse(TEXT("Prompt may be rejected"), Registry.EmitPromptSubmitting(Prompt, Reason));
    TestEqual(TEXT("Veto reason is preserved"), Reason, TEXT("vetoed"));
    TestTrue(TEXT("Veto hook unregisters"), Registry.Unregister(Handles.Pop()));

    Handles.Add(Registry.RegisterPromptSubmitting(TEXT("Test.Hooks.MutateInput"), -20,
        [](FBSHookPromptContext& Context, FString&)
        {
            Context.UserPrompt = TEXT("changed");
            return true;
        }));
    TestFalse(TEXT("User input is immutable"), Registry.EmitPromptSubmitting(Prompt, Reason));
    TestTrue(TEXT("Immutable field error identifies hook"), Reason.Contains(TEXT("Test.Hooks.MutateInput")));
    TestTrue(TEXT("Mutation hook unregisters"), Registry.Unregister(Handles.Pop()));

    FBSHookToolContext Tool;
    Tool.InvocationId = FGuid::NewGuid();
    Tool.RunId = Prompt.RunId;
    Tool.TargetName = TEXT("test.tool");
    Handles.Add(Registry.RegisterToolBefore(TEXT("Test.Hooks.Before1"), -10,
        [&Order](const FBSHookToolContext&, FBSHookToolBeforeContinuation Continue)
        {
            Order.Add(TEXT("before1"));
            FBSHookToolBeforeResult Result;
            Result.bNeedsVisibleEditorFrame = true;
            Continue(MoveTemp(Result));
        }));
    Handles.Add(Registry.RegisterToolBefore(TEXT("Test.Hooks.Before2"), 10,
        [&Order](const FBSHookToolContext&, FBSHookToolBeforeContinuation Continue)
        {
            Order.Add(TEXT("before2"));
            FBSHookToolBeforeResult Result;
            Result.Decision = EBSHookToolDecision::Handled;
            Result.Result = FMCPToolResult::Success(TEXT("handled"));
            Continue(MoveTemp(Result));
        }));
    bool bCompletedSynchronously = false;
    Registry.RunToolBefore(Tool,
        [&bCompletedSynchronously, this](FBSHookToolBeforeResult Result)
        {
            bCompletedSynchronously = true;
            TestTrue(TEXT("Handler may replace tool result"),
                Result.Decision == EBSHookToolDecision::Handled);
            TestTrue(TEXT("Visible frame request accumulates"), Result.bNeedsVisibleEditorFrame);
        });
    TestTrue(TEXT("Game Thread continuation completes inline"), bCompletedSynchronously);
    TestTrue(TEXT("Before hooks keep priority order"),
        FString::Join(Order, TEXT(",")).EndsWith(TEXT("before1,before2")));

    Handles.Add(Registry.RegisterToolReady(TEXT("Test.Hooks.Ready"), 0,
        [&Order](const FBSHookToolContext&) { Order.Add(TEXT("ready")); return true; }));
    TestTrue(TEXT("Ready requests a visible frame"), Registry.EmitToolReady(Tool));
    Handles.Add(Registry.RegisterToolAfter(TEXT("Test.Hooks.After"), 0,
        [&Order](const FBSHookToolContext& Context, const FBSHookToolOutcome& Outcome)
        {
            if (Context.InvocationId.IsValid() && Outcome.bWasDispatched) { Order.Add(TEXT("after")); }
        }));
    FBSHookToolOutcome Outcome;
    Outcome.Kind = EBSHookToolOutcomeKind::Dispatched;
    Outcome.bWasDispatched = true;
    Registry.EmitToolAfter(Tool, Outcome);
    TestTrue(TEXT("After notification fires"), FString::Join(Order, TEXT(",")).EndsWith(TEXT("ready,after")));

    int32 ExitCount = 0;
    Handles.Add(Registry.RegisterRunExited(TEXT("Test.Hooks.Exit"), 0,
        [&ExitCount](const FBSHookRunExitContext&) { ++ExitCount; }));
    FBSHookRunExitContext Exit;
    Exit.RunId = Prompt.RunId;
    Registry.EmitRunExited(Exit);
    TestEqual(TEXT("Run exit notification fires"), ExitCount, 1);

    for (const FBSHookHandle Handle : Handles)
    {
        TestTrue(TEXT("Registered hook can be removed"), Registry.Unregister(Handle));
        TestFalse(TEXT("Hook is removed once"), Registry.Unregister(Handle));
    }
    return true;
}

class FBSHarnessAsyncHookCommand final : public IAutomationLatentCommand
{
public:
    explicit FBSHarnessAsyncHookCommand(FAutomationTestBase* InTest) : Test(InTest)
    {
        FBSHarnessHookRegistry& Registry = FBSHarnessHookRegistry::Get();
        Handle = Registry.RegisterToolBefore(TEXT("Test.Hooks.WorkerContinuation"), 0,
            [this](const FBSHookToolContext&, FBSHookToolBeforeContinuation Continue)
            {
                HeldContinuation = MakeShared<FBSHookToolBeforeContinuation,
                    ESPMode::ThreadSafe>(MoveTemp(Continue));
            });
        FBSHookToolContext Context;
        Context.InvocationId = FGuid::NewGuid();
        Registry.RunToolBefore(Context, [State = Observation](FBSHookToolBeforeResult)
        {
            State->bDeliveredOnGameThread = IsInGameThread();
            ++State->CompletionCount;
        });
        StartedAt = FPlatformTime::Seconds();
        const TSharedPtr<FBSHookToolBeforeContinuation, ESPMode::ThreadSafe> Continuation = HeldContinuation;
        Worker = Async(EAsyncExecution::ThreadPool, [Continuation]()
        {
            FBSHookToolBeforeResult Result;
            (*Continuation)(Result);
            (*Continuation)(Result);
        });
    }

    virtual bool Update() override
    {
        if ((Observation->CompletionCount == 0 || !Worker.IsReady())
            && FPlatformTime::Seconds() - StartedAt < 5.0)
        {
            return false;
        }
        Test->TestTrue(TEXT("Worker continuation finished"), Worker.IsReady());
        Test->TestEqual(TEXT("Duplicate continuation ignored"), Observation->CompletionCount, 1);
        Test->TestTrue(TEXT("Worker continuation delivered on Game Thread"), Observation->bDeliveredOnGameThread);
        Test->TestTrue(TEXT("Async hook unregistered"),
            FBSHarnessHookRegistry::Get().Unregister(Handle));
        return true;
    }

private:
    struct FObservation
    {
        int32 CompletionCount = 0;
        bool bDeliveredOnGameThread = false;
    };
    FAutomationTestBase* Test;
    FBSHookHandle Handle;
    TSharedPtr<FBSHookToolBeforeContinuation, ESPMode::ThreadSafe> HeldContinuation;
    TSharedRef<FObservation, ESPMode::ThreadSafe> Observation =
        MakeShared<FObservation, ESPMode::ThreadSafe>();
    TFuture<void> Worker;
    double StartedAt = 0.0;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBSHarnessHookRegistryAsyncTest,
    "BSHarness.Hooks.Registry.AsyncContinuation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBSHarnessHookRegistryAsyncTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FBSHarnessAsyncHookCommand(this));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBSHarnessHookRegistryCancellationTest,
    "BSHarness.Hooks.Registry.CancelPendingInvocation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBSHarnessHookRegistryCancellationTest::RunTest(const FString& Parameters)
{
    FBSHarnessHookRegistry& Registry = FBSHarnessHookRegistry::Get();
    TArray<FBSHookToolBeforeContinuation> HeldContinuations;
    const FBSHookHandle Handle = Registry.RegisterToolBefore(
        TEXT("Test.Hooks.CancelPending"), -100,
        [&HeldContinuations](const FBSHookToolContext&, FBSHookToolBeforeContinuation Continue)
        {
            HeldContinuations.Add(MoveTemp(Continue));
        });
    TestTrue(TEXT("Pending hook registered"), Handle.IsValid());

    FBSHookToolContext Cancelled;
    Cancelled.InvocationId = FGuid::NewGuid();
    FBSHookToolContext Other;
    Other.InvocationId = FGuid::NewGuid();
    int32 CompletionCount = 0;
    TSharedPtr<int32> FirstPayload = MakeShared<int32>(1);
    TSharedPtr<int32> SecondPayload = MakeShared<int32>(2);
    TWeakPtr<int32> WeakFirst = FirstPayload;
    TWeakPtr<int32> WeakSecond = SecondPayload;
    Registry.RunToolBefore(Cancelled,
        [Keep = FirstPayload, &CompletionCount](FBSHookToolBeforeResult)
        {
            ++CompletionCount;
        });
    Registry.RunToolBefore(Cancelled,
        [Keep = SecondPayload, &CompletionCount](FBSHookToolBeforeResult)
        {
            ++CompletionCount;
        });
    Registry.RunToolBefore(Other,
        [&CompletionCount](FBSHookToolBeforeResult)
        {
            ++CompletionCount;
        });
    FirstPayload.Reset();
    SecondPayload.Reset();
    TestEqual(TEXT("Three continuations are pending"), HeldContinuations.Num(), 3);
    TestTrue(TEXT("First Workbench completion is retained before cancel"), WeakFirst.IsValid());
    TestTrue(TEXT("Second Workbench completion is retained before cancel"), WeakSecond.IsValid());

    Registry.CancelToolInvocation(Cancelled.InvocationId);
    TestFalse(TEXT("First Workbench completion is released on cancel"), WeakFirst.IsValid());
    TestFalse(TEXT("Second Workbench completion is released on cancel"), WeakSecond.IsValid());
    FBSHookToolBeforeResult ContinueResult;
    HeldContinuations[0](ContinueResult);
    HeldContinuations[1](ContinueResult);
    TestEqual(TEXT("Late continuations are ignored"), CompletionCount, 0);
    HeldContinuations[2](ContinueResult);
    TestEqual(TEXT("Another invocation remains active"), CompletionCount, 1);
    Registry.CancelToolInvocation(Other.InvocationId);
    TestTrue(TEXT("Cancellation hook unregistered"), Registry.Unregister(Handle));
    return true;
}

#endif
