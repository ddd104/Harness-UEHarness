#include "AgentExecutionTreeModel.h"

#include "AgentWorkbenchSession.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif

namespace
{
constexpr int32 MaxQuestionPreviewChars = 45;

FString QuestionPreview(const FString& Input)
{
    FString Normalized;
    Normalized.Reserve(Input.Len());
    bool bPendingSpace = false;
    for (int32 Index = 0; Index < Input.Len(); ++Index)
    {
        const TCHAR Character = Input[Index];
        if (FChar::IsWhitespace(Character))
        {
            bPendingSpace = !Normalized.IsEmpty();
            continue;
        }
        if (bPendingSpace) { Normalized.AppendChar(TEXT(' ')); }
        Normalized.AppendChar(Character);
        bPendingSpace = false;
    }
    if (Normalized.Len() > MaxQuestionPreviewChars)
    {
        int32 End = MaxQuestionPreviewChars;
        if (sizeof(TCHAR) == 2 && End < Normalized.Len())
        {
            const uint32 LastIncluded = static_cast<uint32>(Normalized[End - 1]);
            const uint32 FirstExcluded = static_cast<uint32>(Normalized[End]);
            if (LastIncluded >= 0xD800 && LastIncluded <= 0xDBFF
                && FirstExcluded >= 0xDC00 && FirstExcluded <= 0xDFFF)
            { --End; }
        }
        return Normalized.Left(End) + TEXT("…");
    }
    return Normalized;
}

FString FindQuestionPreview(const FAgentSession& Session, const FGuid& RunId)
{
    for (const TSharedPtr<FAgentMessage>& Message : Session.Messages)
    {
        if (Message && Message->RunId == RunId && Message->Role == EAgentMessageRole::User)
        {
            const FString Preview = QuestionPreview(Message->Text);
            if (!Preview.IsEmpty()) { return Preview; }
        }
    }
    for (const FAgentRunInputSnapshot& Run : Session.RunHistory)
    {
        if (Run.RunId == RunId)
        {
            const FString Preview = QuestionPreview(Run.UserInput);
            if (!Preview.IsEmpty()) { return Preview; }
        }
    }
    return TEXT("未记录用户输入");
}
}

TArray<TSharedPtr<FAgentExecutionNode>> FAgentExecutionTreeModel::Build(const FAgentSession& Session)
{
    TArray<TSharedPtr<FAgentExecutionNode>> Roots;
    TMap<FGuid, TSharedPtr<FAgentExecutionNode>> Runs;
    int32 RunNumber = 0;
    for (const TSharedPtr<FAgentEvent>& Event : Session.Events)
    {
        if (!Event) { continue; }

        TSharedPtr<FAgentExecutionNode>* Found = Runs.Find(Event->RunId);
        TSharedPtr<FAgentExecutionNode> Run;
        if (Found)
        {
            Run = *Found;
        }
        else
        {
            Run = MakeShared<FAgentExecutionNode>();
            Run->Kind = EAgentExecutionNodeKind::Run;
            Run->RunId = Event->RunId;
            Run->Sequence = Event->Sequence;
            Run->Title = Event->RunId.IsValid()
                ? FString::Printf(TEXT("第 %d 轮 · %s"), ++RunNumber, *FindQuestionPreview(Session, Event->RunId))
                : TEXT("历史执行记录");
            Runs.Add(Event->RunId, Run);
            Roots.Add(Run);
        }

        TSharedPtr<FAgentExecutionNode> Step = MakeShared<FAgentExecutionNode>();
        Step->Kind = EAgentExecutionNodeKind::Step;
        Step->RunId = Event->RunId;
        Step->Sequence = Event->Sequence;
        Step->Title = Event->Summary;
        Step->Event = Event;
        if (!Event->Detail.IsEmpty())
        {
            TSharedPtr<FAgentExecutionNode> Detail = MakeShared<FAgentExecutionNode>();
            Detail->Kind = EAgentExecutionNodeKind::Detail;
            Detail->RunId = Event->RunId;
            Detail->Sequence = Event->Sequence;
            Detail->Title = TEXT("详情");
            Detail->Event = Event;
            Step->Children.Add(Detail);
        }
        Run->Children.Add(Step);
    }
    return Roots;
}

EAgentExecutionRunStatus FAgentExecutionTreeModel::StatusFor(
    const FAgentSession& Session, const FAgentExecutionNode& Run)
{
    if (Run.Kind != EAgentExecutionNodeKind::Run || !Run.RunId.IsValid())
    { return EAgentExecutionRunStatus::Unknown; }
    for (int32 Index = Run.Children.Num() - 1; Index >= 0; --Index)
    {
        const TSharedPtr<FAgentExecutionNode>& Step = Run.Children[Index];
        if (!Step || !Step->Event) { continue; }
        switch (Step->Event->Type)
        {
        case EAgentEventType::RunCompleted: return EAgentExecutionRunStatus::Completed;
        case EAgentEventType::RunFailed: return EAgentExecutionRunStatus::Failed;
        case EAgentEventType::RunCancelled: return EAgentExecutionRunStatus::Cancelled;
        default: break;
        }
    }
    if (Session.Runner.CurrentRunId == Run.RunId && Session.Runner.State == EAgentRunState::Running)
    { return EAgentExecutionRunStatus::Running; }
    if (Session.RunHistory.ContainsByPredicate([&Run](const FAgentRunInputSnapshot& Snapshot)
        { return Snapshot.RunId == Run.RunId; }))
    { return EAgentExecutionRunStatus::Interrupted; }
    return EAgentExecutionRunStatus::Unknown;
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentExecutionTreeModelTest,
    "AgentWorkbench.Display.ExecutionTreeModel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentExecutionTreeModelTest::RunTest(const FString& Parameters)
{
    FAgentSession Session;
    const FGuid FirstRun = FGuid::NewGuid();
    const FGuid SecondRun = FGuid::NewGuid();

    TSharedPtr<FAgentMessage> User = MakeShared<FAgentMessage>();
    User->Role = EAgentMessageRole::User;
    User->RunId = FirstRun;
    User->Text = TEXT("  请检查\n 蓝图变量 \t及其默认值   ");
    Session.Messages.Add(User);

    FAgentRunInputSnapshot FirstSnapshot;
    FirstSnapshot.RunId = FirstRun;
    FirstSnapshot.UserInput = TEXT("旧快照的问题不应覆盖用户消息");
    Session.RunHistory.Add(FirstSnapshot);
    FAgentRunInputSnapshot SecondSnapshot;
    SecondSnapshot.RunId = SecondRun;
    SecondSnapshot.UserInput = FString::ChrN(50, TEXT('B'));
    Session.RunHistory.Add(SecondSnapshot);

    auto AddEvent = [&Session](const FGuid& RunId, int32 Sequence,
        const FString& Summary, const FString& Detail = FString())
    {
        TSharedPtr<FAgentEvent> Event = MakeShared<FAgentEvent>();
        Event->RunId = RunId;
        Event->Sequence = Sequence;
        Event->Summary = Summary;
        Event->Detail = Detail;
        Session.Events.Add(Event);
        return Event;
    };
    const TSharedPtr<FAgentEvent> FirstStart = AddEvent(FirstRun, 10, TEXT("任务开始"));
    const TSharedPtr<FAgentEvent> LegacyStep = AddEvent(FGuid(), 11, TEXT("旧步骤"), TEXT("旧详情"));
    AddEvent(SecondRun, 12, TEXT("任务开始"));
    const TSharedPtr<FAgentEvent> FirstTool = AddEvent(FirstRun, 13,
        TEXT("调用工具：BlueprintTools.list_variables"), TEXT("{}"));
    AddEvent(SecondRun, 14, TEXT("任务完成"));
    AddEvent(FGuid(), 15, TEXT("旧步骤结束"));

    const TArray<TSharedPtr<FAgentExecutionNode>> Roots = FAgentExecutionTreeModel::Build(Session);
    TestEqual(TEXT("Run groups follow first event appearance"), Roots.Num(), 3);
    if (Roots.Num() != 3) { return false; }
    TestEqual(TEXT("First run title uses normalized user question"), Roots[0]->Title,
        FString(TEXT("第 1 轮 · 请检查 蓝图变量 及其默认值")));
    TestEqual(TEXT("Legacy events get a single explicit group"), Roots[1]->Title,
        FString(TEXT("历史执行记录")));
    TestEqual(TEXT("Second run falls back to the snapshot and truncates its title"), Roots[2]->Title,
        FString(TEXT("第 2 轮 · ")) + FString::ChrN(45, TEXT('B')) + TEXT("…"));
    TestFalse(TEXT("Visible run titles hide run IDs"), Roots[0]->Title.Contains(FirstRun.ToString()));
    TestFalse(TEXT("Visible run titles hide run IDs"), Roots[2]->Title.Contains(SecondRun.ToString()));
    TestEqual(TEXT("First run retains its two events"), Roots[0]->Children.Num(), 2);
    TestEqual(TEXT("Legacy group retains its two events"), Roots[1]->Children.Num(), 2);
    TestEqual(TEXT("Second run retains its two events"), Roots[2]->Children.Num(), 2);
    if (Roots[0]->Children.Num() != 2 || Roots[1]->Children.Num() != 2
        || Roots[2]->Children.Num() != 2) { return false; }

    TestEqual(TEXT("Step title is exactly the event summary"), Roots[0]->Children[1]->Title,
        FirstTool->Summary);
    TestEqual(TEXT("Step sequence is preserved"), Roots[0]->Children[1]->Sequence, 13);
    TestTrue(TEXT("Step references its original event"), Roots[0]->Children[1]->Event == FirstTool);
    TestTrue(TEXT("Empty detail adds no node"), Roots[0]->Children[0]->Children.IsEmpty());
    TestTrue(TEXT("Event without detail remains unchanged"), FirstStart->Detail.IsEmpty());
    TestEqual(TEXT("Nonempty detail adds one node"), Roots[0]->Children[1]->Children.Num(), 1);
    TestEqual(TEXT("Legacy nonempty detail adds one node"), Roots[1]->Children[0]->Children.Num(), 1);
    if (Roots[0]->Children[1]->Children.Num() == 1)
    {
        const TSharedPtr<FAgentExecutionNode>& Detail = Roots[0]->Children[1]->Children[0];
        TestEqual(TEXT("Detail node has its own kind"), Detail->Kind, EAgentExecutionNodeKind::Detail);
        TestTrue(TEXT("Detail references the original event"), Detail->Event == FirstTool);
        TestEqual(TEXT("Detail text is not copied into the tree"), FirstTool->Detail, FString(TEXT("{}")));
    }
    TestTrue(TEXT("Legacy step keeps its event"), Roots[1]->Children[0]->Event == LegacyStep);
    TestEqual(TEXT("Source events remain unchanged"), Session.Events.Num(), 6);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentExecutionTreeStatusTest,
    "AgentWorkbench.Display.ExecutionTreeStatus",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentExecutionTreeStatusTest::RunTest(const FString& Parameters)
{
    FAgentSession Session;
    const FGuid InterruptedRun = FGuid::NewGuid();
    const FGuid CurrentRun = FGuid::NewGuid();
    const FGuid CompletedRun = FGuid::NewGuid();
    const FGuid FailedRun = FGuid::NewGuid();
    const FGuid CancelledRun = FGuid::NewGuid();
    const FGuid UnknownRun = FGuid::NewGuid();
    for (const FGuid& RunId : {InterruptedRun, CurrentRun})
    {
        FAgentRunInputSnapshot Snapshot;
        Snapshot.RunId = RunId;
        Snapshot.UserInput = TEXT("问题");
        Session.RunHistory.Add(MoveTemp(Snapshot));
    }
    auto AddEvent = [&Session](const FGuid& RunId, EAgentEventType Type)
    {
        TSharedPtr<FAgentEvent> Event = MakeShared<FAgentEvent>();
        Event->RunId = RunId;
        Event->Type = Type;
        Event->Sequence = Session.Events.Num() + 1;
        Event->Summary = TEXT("步骤");
        Session.Events.Add(Event);
    };
    AddEvent(InterruptedRun, EAgentEventType::RunStarted);
    AddEvent(CurrentRun, EAgentEventType::RunStarted);
    AddEvent(CompletedRun, EAgentEventType::RunCompleted);
    AddEvent(FailedRun, EAgentEventType::RunFailed);
    AddEvent(CancelledRun, EAgentEventType::RunCancelled);
    AddEvent(UnknownRun, EAgentEventType::RunStarted);
    AddEvent(FGuid(), EAgentEventType::RunStarted);
    Session.Runner.CurrentRunId = CurrentRun;
    Session.Runner.State = EAgentRunState::Running;

    const TArray<TSharedPtr<FAgentExecutionNode>> Roots = FAgentExecutionTreeModel::Build(Session);
    TestEqual(TEXT("All status fixtures are grouped"), Roots.Num(), 7);
    if (Roots.Num() != 7) { return false; }
    TestEqual(TEXT("Previous unfinished run stays interrupted after next run begins"),
        FAgentExecutionTreeModel::StatusFor(Session, *Roots[0]), EAgentExecutionRunStatus::Interrupted);
    TestEqual(TEXT("Current active run is running"),
        FAgentExecutionTreeModel::StatusFor(Session, *Roots[1]), EAgentExecutionRunStatus::Running);
    TestEqual(TEXT("Completed terminal event wins"),
        FAgentExecutionTreeModel::StatusFor(Session, *Roots[2]), EAgentExecutionRunStatus::Completed);
    TestEqual(TEXT("Failed terminal event wins"),
        FAgentExecutionTreeModel::StatusFor(Session, *Roots[3]), EAgentExecutionRunStatus::Failed);
    TestEqual(TEXT("Cancelled terminal event wins"),
        FAgentExecutionTreeModel::StatusFor(Session, *Roots[4]), EAgentExecutionRunStatus::Cancelled);
    TestEqual(TEXT("Orphan run without snapshot has unknown status"),
        FAgentExecutionTreeModel::StatusFor(Session, *Roots[5]), EAgentExecutionRunStatus::Unknown);
    TestEqual(TEXT("Legacy group with no run ID has unknown status"),
        FAgentExecutionTreeModel::StatusFor(Session, *Roots[6]), EAgentExecutionRunStatus::Unknown);
    Session.Runner.CurrentRunId = CompletedRun;
    TestEqual(TEXT("Terminal event takes priority over active runner state"),
        FAgentExecutionTreeModel::StatusFor(Session, *Roots[2]), EAgentExecutionRunStatus::Completed);

    if (sizeof(TCHAR) == 2)
    {
        FAgentSession EmojiSession;
        const FGuid EmojiRun = FGuid::NewGuid();
        TSharedPtr<FAgentMessage> EmojiQuestion = MakeShared<FAgentMessage>();
        EmojiQuestion->Role = EAgentMessageRole::User;
        EmojiQuestion->RunId = EmojiRun;
        EmojiQuestion->Text = FString::ChrN(44, TEXT('A'));
        EmojiQuestion->Text.AppendChar(static_cast<TCHAR>(0xD83D));
        EmojiQuestion->Text.AppendChar(static_cast<TCHAR>(0xDE80));
        EmojiQuestion->Text.AppendChar(TEXT('Z'));
        EmojiSession.Messages.Add(EmojiQuestion);
        TSharedPtr<FAgentEvent> EmojiEvent = MakeShared<FAgentEvent>();
        EmojiEvent->RunId = EmojiRun;
        EmojiSession.Events.Add(EmojiEvent);
        const TArray<TSharedPtr<FAgentExecutionNode>> EmojiRoots = FAgentExecutionTreeModel::Build(EmojiSession);
        TestEqual(TEXT("Emoji fixture creates one run"), EmojiRoots.Num(), 1);
        if (EmojiRoots.Num() == 1)
        {
            TestEqual(TEXT("Question preview does not split a UTF-16 surrogate pair"),
                EmojiRoots[0]->Title, FString(TEXT("第 1 轮 · ")) + FString::ChrN(44, TEXT('A')) + TEXT("…"));
        }
    }
    return true;
}
#endif
