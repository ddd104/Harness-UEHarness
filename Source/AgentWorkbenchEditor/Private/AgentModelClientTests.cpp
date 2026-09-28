#include "AgentWorkbenchSession.h"
#include "AgentWorkbenchSettings.h"
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

#endif
