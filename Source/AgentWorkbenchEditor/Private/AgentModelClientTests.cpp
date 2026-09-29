#include "AgentWorkbenchSession.h"
#include "AgentWorkbenchSettings.h"
#include "AgentToolBridge.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Run explicitly when a configured provider and its credential are available.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentWorkbenchLiveProviderTest,
    "ManualProvider.LiveRequest",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentWorkbenchLiveProviderTest::RunTest(const FString& Parameters)
{
    TSharedRef<FAgentSession> Session = MakeShared<FAgentSession>();
    Session->ModelOptions.Model = GetDefault<UAgentWorkbenchSettings>()->DefaultModel;
    Session->ModelOptions.MaxOutputTokens = 512;
    Session->DraftText = TEXT("Reply with OK only.");
    FString Error;
    if (!Session->Send(Error))
    {
        AddError(Error);
        return false;
    }
    const double Deadline = FPlatformTime::Seconds() + 90.0;
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Session, Deadline]()
    {
        if (Session->Runner.State == EAgentRunState::Running && FPlatformTime::Seconds() < Deadline) { return false; }
        if (Session->Runner.State == EAgentRunState::Running)
        {
            Session->CancelRun();
            AddError(TEXT("Live model request timed out."));
            return true;
        }
        TestEqual(TEXT("Live request completed"), Session->Runner.State, EAgentRunState::Completed);
        if (Session->Runner.State == EAgentRunState::Completed)
        {
            TestTrue(TEXT("Provider returned actual text"), Session->Messages.Num() == 2
                && !Session->Messages[1]->Text.IsEmpty());
        }
        else if (!Session->Messages.IsEmpty()) { AddError(Session->Messages.Last()->Text); }
        return true;
    }));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentWorkbenchLiveToolCallTest,
    "ManualProvider.LiveToolCall",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentWorkbenchLiveToolCallTest::RunTest(const FString& Parameters)
{
    TSharedRef<FAgentSession> Session = MakeShared<FAgentSession>();
    Session->ModelOptions.Model = GetDefault<UAgentWorkbenchSettings>()->DefaultModel;
    Session->ModelOptions.MaxOutputTokens = 512;
    Session->DraftText = FString::Printf(TEXT("Call the %s tool to check the running Unreal Engine version, "
        "then answer in one short sentence using the returned version. Do not guess."),
        *FAgentToolBridge::ToModelName(TEXT("bsharness.editor_info")));
    FString Error;
    if (!Session->Send(Error))
    {
        AddError(Error);
        return false;
    }
    const double Deadline = FPlatformTime::Seconds() + 120.0;
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Session, Deadline]()
    {
        if (Session->Runner.State == EAgentRunState::Running && FPlatformTime::Seconds() < Deadline) { return false; }
        if (Session->Runner.State == EAgentRunState::Running)
        {
            Session->CancelRun();
            AddError(TEXT("Live tool call timed out."));
            return true;
        }
        TestEqual(TEXT("Run completed after tool result"), Session->Runner.State, EAgentRunState::Completed);
        int32 Started = 0;
        int32 Finished = 0;
        for (const TSharedPtr<FAgentEvent>& Event : Session->Events)
        {
            if (!Event) { continue; }
            if (Event->Type == EAgentEventType::ToolCallStarted) { ++Started; }
            if (Event->Type == EAgentEventType::ToolCallCompleted) { ++Finished; }
        }
        TestTrue(TEXT("Model requested a real tool"), Started >= 1);
        TestEqual(TEXT("Every requested tool returned"), Finished, Started);
        TestTrue(TEXT("Final reply follows tool result"), !Session->Messages.IsEmpty()
            && Session->Messages.Last()->Role == EAgentMessageRole::Assistant
            && !Session->Messages.Last()->Text.IsEmpty());
        if (Session->Runner.State != EAgentRunState::Completed && !Session->Messages.IsEmpty())
        { AddError(Session->Messages.Last()->Text); }
        return true;
    }));
    return true;
}

#endif
