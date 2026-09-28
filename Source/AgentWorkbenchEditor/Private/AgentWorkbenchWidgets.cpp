#include "AgentWorkbenchWidgets.h"
#include "AgentAssetContextService.h"
#include "AgentWorkbenchSettings.h"
#include "AgentModelClient.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Containers/Ticker.h"
#include "Misc/MessageDialog.h"
#include "InputCoreTypes.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Text/SInlineEditableTextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "AgentWorkbench"

void SAgentHistoryPanel::Construct(const FArguments& Args, TSharedRef<FAgentSessionManager> InManager, const FGuid& InCurrentSessionId)
{
    Manager = InManager;
    CurrentSessionId = InCurrentSessionId;
    Refresh();
    Manager->OnSessionsChanged.AddSP(this, &SAgentHistoryPanel::Refresh);
    ChildSlot [ SNew(SBorder).Padding(8) [
        SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8) [ SNew(STextBlock).Text(LOCTEXT("History", "历史对话")) ]
        + SVerticalBox::Slot().AutoHeight().Padding(0, 8) [ SNew(SEditableTextBox)
            .HintText(LOCTEXT("HistorySearch", "搜索历史会话"))
            .OnTextChanged_Lambda([this](const FText& Text) { Search = Text.ToString(); Refresh(); }) ]
        + SVerticalBox::Slot().FillHeight(1) [ SAssignNew(List, SListView<TSharedPtr<FAgentSession>>)
            .ListItemsSource(&Filtered).OnGenerateRow(this, &SAgentHistoryPanel::MakeRow)
            .SelectionMode(ESelectionMode::Single)
            .OnMouseButtonClick_Lambda([this](TSharedPtr<FAgentSession> Item)
            { if (Item) { Manager->RequestSwitchSession(CurrentSessionId, Item.ToSharedRef()); } })
            .OnContextMenuOpening(this, &SAgentHistoryPanel::MakeContextMenu)
            .OnKeyDownHandler(this, &SAgentHistoryPanel::OnHistoryKeyDown) ]
    ] ];
    for (const TSharedPtr<FAgentSession>& Item : Filtered)
    { if (Item && Item->SessionId == CurrentSessionId) { List->SetSelection(Item); break; } }
}

void SAgentHistoryPanel::Refresh()
{
    Filtered.Reset();
    if (Manager)
    {
        for (const TSharedPtr<FAgentSession>& Item : Manager->GetHistorySessions())
        {
            if (Item && (Search.IsEmpty() || Item->Title.Contains(Search))) { Filtered.Add(Item); }
        }
    }
    if (List) { List->RequestListRefresh(); }
}

TSharedRef<ITableRow> SAgentHistoryPanel::MakeRow(TSharedPtr<FAgentSession> Item, const TSharedRef<STableViewBase>& Owner)
{
    TSharedPtr<SInlineEditableTextBlock> NameWidget;
    TSharedRef<ITableRow> Row = SNew(STableRow<TSharedPtr<FAgentSession>>, Owner)
        .ToolTipText(FText::FromString(Item->SessionId.ToString())) [ SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight() [ SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth() [ SNew(STextBlock).Text(FText::FromString(
                    Item->SessionId == CurrentSessionId ? TEXT("● ") : TEXT("○ "))) ]
                + SHorizontalBox::Slot().FillWidth(1) [ SAssignNew(NameWidget, SInlineEditableTextBlock)
                .Text_Lambda([Item]() { return FText::FromString(Item->Title); })
                .MaximumLength(128)
                .DelayedLeftClickEntersEditMode(false)
                .OnVerifyTextChanged_Lambda([](const FText& Text, FText& Error)
                {
                    const FString Title = Text.ToString().TrimStartAndEnd();
                    if (!Title.IsEmpty() && !Title.Contains(TEXT("\n")) && !Title.Contains(TEXT("\r"))) { return true; }
                    Error = LOCTEXT("InvalidHistoryTitle", "对话标题不能为空或包含换行");
                    return false;
                })
                .OnTextCommitted_Lambda([this, Item](const FText& Text, ETextCommit::Type Commit)
                { if (Commit != ETextCommit::OnCleared) { Manager->RenameSession(Item.ToSharedRef(), Text.ToString()); } }) ] ]
            + SVerticalBox::Slot().AutoHeight() [ SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("%s UTC"),
                *Item->UpdatedAt.ToString()))) ] ];
    RenameWidgets.Add(Item->SessionId, NameWidget);
    return Row;
}

void SAgentHistoryPanel::FocusList()
{
    if (List) { FSlateApplication::Get().SetKeyboardFocus(List, EFocusCause::SetDirectly); }
}

void SAgentHistoryPanel::BeginRename(const TSharedRef<FAgentSession>& Item)
{
    if (const TWeakPtr<SInlineEditableTextBlock>* Found = RenameWidgets.Find(Item->SessionId))
    { if (TSharedPtr<SInlineEditableTextBlock> Widget = Found->Pin()) { Widget->EnterEditingMode(); } }
}

void SAgentHistoryPanel::ConfirmDelete(const TSharedRef<FAgentSession>& Item)
{
    if (Item->Runner.State == EAgentRunState::Running) { return; }
    const FText Prompt = FText::FromString(FString::Printf(TEXT("删除对话“%s”及其历史记录？候选资产本身不会删除。"), *Item->Title));
    if (FMessageDialog::Open(EAppMsgType::YesNo, Prompt) == EAppReturnType::Yes)
    { Manager->RequestDeleteSession(Item); }
}

FReply SAgentHistoryPanel::OnHistoryKeyDown(const FGeometry&, const FKeyEvent& Event)
{
    if (Event.IsAltDown() || Event.IsControlDown() || Event.IsShiftDown() || !List) { return FReply::Unhandled(); }
    const TArray<TSharedPtr<FAgentSession>> Selected = List->GetSelectedItems();
    if (Selected.Num() != 1 || !Selected[0]) { return FReply::Unhandled(); }
    if (Event.GetKey() == EKeys::F2) { BeginRename(Selected[0].ToSharedRef()); return FReply::Handled(); }
    if (Event.GetKey() == EKeys::Delete) { ConfirmDelete(Selected[0].ToSharedRef()); return FReply::Handled(); }
    return FReply::Unhandled();
}

TSharedPtr<SWidget> SAgentHistoryPanel::MakeContextMenu()
{
    if (!List) { return nullptr; }
    const TArray<TSharedPtr<FAgentSession>> Selected = List->GetSelectedItems();
    if (Selected.Num() != 1 || !Selected[0]) { return nullptr; }
    const TSharedRef<FAgentSession> Item = Selected[0].ToSharedRef();
    const FGuid HostSessionId = CurrentSessionId;
    const TSharedPtr<FAgentSessionManager> SessionManager = Manager;
    TWeakPtr<SAgentHistoryPanel> WeakPanel = SharedThis(this);
    FMenuBuilder Menu(true, nullptr);
    Menu.AddMenuEntry(LOCTEXT("RenameHistory", "重命名"),
        LOCTEXT("RenameHistoryTip", "修改此对话标题（F2）"), FSlateIcon(),
        FUIAction(FExecuteAction::CreateLambda([WeakPanel, Item]()
        {
            FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakPanel, Item](float)
            { if (TSharedPtr<SAgentHistoryPanel> Panel = WeakPanel.Pin()) { Panel->BeginRename(Item); } return false; }));
        })));
    Menu.AddMenuEntry(LOCTEXT("DeleteHistory", "删除"),
        LOCTEXT("DeleteHistoryTip", "删除此会话及保存的历史；不会删除候选资产（Delete）"), FSlateIcon(),
        FUIAction(FExecuteAction::CreateLambda([WeakPanel, Item]()
        { if (TSharedPtr<SAgentHistoryPanel> Panel = WeakPanel.Pin()) { Panel->ConfirmDelete(Item); } }),
            FCanExecuteAction::CreateLambda([Item]() { return Item->Runner.State != EAgentRunState::Running; })));
    Menu.AddMenuEntry(LOCTEXT("ForkHistory", "新建分叉"),
        LOCTEXT("ForkHistoryTip", "复制此会话并在当前窗口切换到新分叉"), FSlateIcon(),
        FUIAction(FExecuteAction::CreateLambda([SessionManager, Item, HostSessionId]()
        {
            const TSharedRef<FAgentSession> Fork = SessionManager->ForkSession(Item, false);
            SessionManager->RequestSwitchSession(HostSessionId, Fork);
        })));
    return Menu.MakeWidget();
}

void SAgentModelOptionsBar::Construct(const FArguments& Args, TSharedRef<FAgentSession> InSession)
{
    Session = InSession;
    if (Session->ModelOptions.Model.IsEmpty() || Session->ModelOptions.Model.Equals(TEXT("Mock"), ESearchCase::IgnoreCase))
    {
        Session->ModelOptions.Model = GetDefault<UAgentWorkbenchSettings>()->DefaultModel;
        Session->Touch();
    }
    RefreshModels();
    ChildSlot [ SNew(SBorder).Padding(8) [ SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
            [ SNew(STextBlock).Text_Lambda([]() { return FText::FromString(TEXT("连接: ") + FAgentModelClient::ProviderName(GetDefault<UAgentWorkbenchSettings>()->ProviderType)); }) ]
        + SHorizontalBox::Slot().FillWidth(1).Padding(0, 0, 8, 0)
            [ SAssignNew(ModelCombo, SComboBox<TSharedPtr<FString>>).OptionsSource(&ModelChoices)
                .OnComboBoxOpening(this, &SAgentModelOptionsBar::RefreshModels)
                .OnGenerateWidget_Lambda([](TSharedPtr<FString> Name) { return SNew(STextBlock).Text(FText::FromString(*Name)); })
                .OnSelectionChanged_Lambda([this](TSharedPtr<FString> Name, ESelectInfo::Type) {
                    if (Name) { Session->ModelOptions.Model = *Name; Session->Touch(); }
                })
                [ SNew(STextBlock).Text_Lambda([this]() { return FText::FromString(Session->ModelOptions.Model); }) ] ]
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
            [ SNew(STextBlock).Text(LOCTEXT("Tokens", "最大输出 Token")) ]
        + SHorizontalBox::Slot().AutoWidth() [ SNew(SBox).WidthOverride(105) [ SNew(SSpinBox<int32>)
            .MinValue(1).MaxValue(100000).Value_Lambda([this]() { return Session->ModelOptions.MaxOutputTokens; })
            .OnValueChanged_Lambda([this](int32 Value) { Session->ModelOptions.MaxOutputTokens = Value; Session->Touch(); }) ] ]
    ] ];
}

void SAgentModelOptionsBar::RefreshModels()
{
    ModelChoices.Empty();
    const UAgentWorkbenchSettings* Settings = GetDefault<UAgentWorkbenchSettings>();
    for (const FString& Name : Settings->AvailableModels)
    {
        if (!Name.IsEmpty() && !Name.Equals(TEXT("Mock"), ESearchCase::IgnoreCase)) { ModelChoices.Add(MakeShared<FString>(Name)); }
    }
    if (!Settings->DefaultModel.IsEmpty() && !Settings->DefaultModel.Equals(TEXT("Mock"), ESearchCase::IgnoreCase)
        && !ModelChoices.ContainsByPredicate([Settings](const TSharedPtr<FString>& Name) { return *Name == Settings->DefaultModel; }))
    { ModelChoices.Add(MakeShared<FString>(Settings->DefaultModel)); }
    if (!ModelChoices.ContainsByPredicate([this](const TSharedPtr<FString>& Name) { return *Name == Session->ModelOptions.Model; }))
    {
        if (!Session->ModelOptions.Model.IsEmpty() && !Session->ModelOptions.Model.Equals(TEXT("Mock"), ESearchCase::IgnoreCase))
        { ModelChoices.Add(MakeShared<FString>(Session->ModelOptions.Model)); }
    }
    if (ModelCombo) { ModelCombo->RefreshOptions(); }
}

void SAgentConversationList::Construct(const FArguments& Args, TSharedRef<FAgentSession> InSession)
{
    Session = InSession;
    ChildSlot [ SNew(SBorder).Padding(8) [ SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6) [ SNew(STextBlock).Text(LOCTEXT("Conversation", "对话记录")) ]
        + SVerticalBox::Slot().FillHeight(1) [ SAssignNew(List, SListView<TSharedPtr<FAgentMessage>>)
            .ListItemsSource(&Session->Messages).OnGenerateRow(this, &SAgentConversationList::MakeRow) ]
    ] ];
}

void SAgentConversationList::Refresh() { if (List) { List->RequestListRefresh(); List->ScrollToBottom(); } }

TSharedRef<ITableRow> SAgentConversationList::MakeRow(TSharedPtr<FAgentMessage> Item, const TSharedRef<STableViewBase>& Owner)
{
    const TCHAR* Role = Item->Role == EAgentMessageRole::User ? TEXT("用户")
        : Item->Role == EAgentMessageRole::Error ? TEXT("错误") : TEXT("Agent");
    FString Body = Item->Text;
    for (const FAgentCandidate& Candidate : Item->IncludedAssets)
    {
        Body += FString::Printf(TEXT("\n[本轮候选] %s"), *Candidate.ObjectPath);
    }
    return SNew(STableRow<TSharedPtr<FAgentMessage>>, Owner) [ SNew(SBorder).Padding(6) [
        SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("%s\n%s"), Role, *Body))).AutoWrapText(true)
    ] ];
}

void SAgentAssetCandidatePanel::Construct(const FArguments& Args, TSharedRef<FAgentSession> InSession)
{
    Session = InSession;
    ChildSlot [ SNew(SBorder).Padding(8) [ SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight() [ SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1) [ SNew(STextBlock).Text_Lambda([this]() {
                return FText::FromString(FString::Printf(TEXT("候选资产 · %d 项"), Session->Candidates.Num()));
            }) ]
            + SHorizontalBox::Slot().AutoWidth() [ SNew(SButton).Text(LOCTEXT("AddAssets", "添加选中资产"))
                .ToolTipText(LOCTEXT("MainBrowserSource", "只读取主内容浏览器当前选中的 /Game/ 资产；不会自动添加其他浏览器或 World Outliner 的选择"))
                .OnClicked(this, &SAgentAssetCandidatePanel::AddSelected) ]
            + SHorizontalBox::Slot().AutoWidth().Padding(6, 0, 0, 0) [ SNew(SButton).Text(LOCTEXT("ClearAssets", "清空候选"))
                .IsEnabled_Lambda([this]() { return !Session->Candidates.IsEmpty(); })
                .OnClicked(this, &SAgentAssetCandidatePanel::Clear) ]
            + SHorizontalBox::Slot().AutoWidth().Padding(6, 0, 0, 0) [ SNew(SButton)
                .Text_Lambda([this]() { return bExpanded ? LOCTEXT("CollapseAssets", "折叠") : LOCTEXT("ExpandAssets", "展开"); })
                .OnClicked_Lambda([this]() { bExpanded = !bExpanded; return FReply::Handled(); }) ] ]
        + SVerticalBox::Slot().AutoHeight() [ SNew(STextBlock).Text(this, &SAgentAssetCandidatePanel::GetFeedbackText).AutoWrapText(true)
            .Visibility_Lambda([this]() { return bExpanded && !GetFeedbackText().IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; }) ]
        + SVerticalBox::Slot().FillHeight(1) [ SAssignNew(List, SListView<TSharedPtr<FAgentCandidate>>)
            .Visibility_Lambda([this]() { return bExpanded && !Session->Candidates.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
            .ListItemsSource(&Session->Candidates).OnGenerateRow(this, &SAgentAssetCandidatePanel::MakeRow) ]
        + SVerticalBox::Slot().FillHeight(1).VAlign(VAlign_Center).HAlign(HAlign_Center)
            [ SNew(STextBlock).Text(LOCTEXT("EmptyCandidates", "尚无候选资产 · 在主内容浏览器选择资产后点击添加"))
                .Visibility_Lambda([this]() { return bExpanded && Session->Candidates.IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; }) ]
        + SVerticalBox::Slot().AutoHeight() [ SNew(STextBlock)
            .Text(LOCTEXT("NextRunChanges", "运行中修改候选只影响下一次发送"))
            .Visibility_Lambda([this]() { return bExpanded && Session->Runner.State == EAgentRunState::Running ? EVisibility::Visible : EVisibility::Collapsed; }) ]
    ] ];
}

void SAgentAssetCandidatePanel::Refresh() { if (List) { List->RequestListRefresh(); } }

FText SAgentAssetCandidatePanel::GetFeedbackText() const
{
    FString Message = Feedback;
    int32 Missing = 0;
    int32 Unknown = 0;
    for (const TSharedPtr<FAgentCandidate>& Candidate : Session->Candidates)
    {
        if (!Candidate) { continue; }
        if (Candidate->ValidationStatus == EAgentCandidateValidation::Missing
            || Candidate->ValidationStatus == EAgentCandidateValidation::Unsupported) { ++Missing; }
        else if (Candidate->ValidationStatus == EAgentCandidateValidation::Unknown) { ++Unknown; }
    }
    if (Missing > 0) { Message += FString::Printf(TEXT(" %d 个候选资产已失效，请点 X 移除后重新选择。"), Missing); }
    if (Unknown > 0) { Message += FString::Printf(TEXT(" %d 个候选资产仍待注册表验证。"), Unknown); }
    return FText::FromString(Message.TrimStartAndEnd());
}

FReply SAgentAssetCandidatePanel::AddSelected()
{
    const FAgentAddCandidatesResult Result = FAgentAssetContextService::AddSelectedAssets(
        *Session, GetDefault<UAgentWorkbenchSettings>()->MaxCandidateAssets);
    Feedback = Result.Message;
    Refresh();
    return FReply::Handled();
}

FReply SAgentAssetCandidatePanel::Clear()
{
    const int32 Count = Session->Candidates.Num();
    Session->Candidates.Empty();
    Session->Touch();
    Feedback = FString::Printf(TEXT("已清空 %d 个候选引用；未修改真实资产。"), Count);
    Refresh();
    return FReply::Handled();
}

TSharedRef<ITableRow> SAgentAssetCandidatePanel::MakeRow(TSharedPtr<FAgentCandidate> Item, const TSharedRef<STableViewBase>& Owner)
{
    return SNew(STableRow<TSharedPtr<FAgentCandidate>>, Owner) [ SNew(SHorizontalBox)
        + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center).Padding(5, 0) [ SNew(STextBlock)
            .Text(FText::FromString(Item->ObjectPath)).ToolTipText(FText::FromString(Item->ObjectPath)).AutoWrapText(true) ]
        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center) [ SNew(SButton).Text(LOCTEXT("RemoveCandidate", "X"))
            .ToolTipText(LOCTEXT("RemoveCandidateTip", "仅移除当前会话中的候选引用，不删除真实资产"))
            .OnClicked_Lambda([this, Item]() {
                Session->Candidates.RemoveSingle(Item);
                Session->Touch();
                Feedback = TEXT("已移除候选引用；未修改真实资产。");
                Refresh();
                return FReply::Handled();
            }) ]
    ];
}

void SAgentExecutionPanel::Construct(const FArguments& Args, TSharedRef<FAgentSession> InSession)
{
    Session = InSession;
    ChildSlot [ SNew(SBorder).Padding(8) [ SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6) [ SNew(STextBlock).Text(LOCTEXT("Execution", "任务执行详情")) ]
        + SVerticalBox::Slot().FillHeight(1) [ SAssignNew(List, SListView<TSharedPtr<FAgentEvent>>)
            .ListItemsSource(&Session->Events).OnGenerateRow(this, &SAgentExecutionPanel::MakeRow) ]
    ] ];
}

void SAgentExecutionPanel::Refresh() { if (List) { List->RequestListRefresh(); List->ScrollToBottom(); } }

TSharedRef<ITableRow> SAgentExecutionPanel::MakeRow(TSharedPtr<FAgentEvent> Item, const TSharedRef<STableViewBase>& Owner)
{
    const FString Label = FString::Printf(TEXT("Run %s · #%d\n%s"),
        *Item->RunId.ToString(EGuidFormats::Digits).Left(8), Item->Sequence, *Item->Summary);
    return SNew(STableRow<TSharedPtr<FAgentEvent>>, Owner) [ SNew(SExpandableArea)
        .InitiallyCollapsed(true)
        .HeaderContent() [ SNew(STextBlock).Text(FText::FromString(Label)).AutoWrapText(true) ]
        .BodyContent() [ SNew(STextBlock).Text(FText::FromString(Item->Detail.IsEmpty() ? TEXT("无详细信息") : Item->Detail)).AutoWrapText(true) ]
    ];
}

void SAgentChatWindow::Construct(const FArguments& Args)
{
    Session = Args._Session;
    Manager = Args._Manager;
    check(Session && Manager);
    Session->OnUiChanged.AddSP(this, &SAgentChatWindow::RefreshAfterRun);
    ChildSlot [ SNew(SVerticalBox)
        + SVerticalBox::Slot().FillHeight(1) [ SNew(SSplitter).Orientation(Orient_Horizontal)
            + SSplitter::Slot().Value(0.20f).MinSize(180) [ SAssignNew(HistoryPanel, SAgentHistoryPanel, Manager.ToSharedRef(), Session->SessionId) ]
            + SSplitter::Slot().Value(0.50f).MinSize(420) [ SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight() [ SNew(SAgentModelOptionsBar, Session.ToSharedRef()) ]
                + SVerticalBox::Slot().FillHeight(1) [ SAssignNew(ConversationList, SAgentConversationList, Session.ToSharedRef()) ]
                + SVerticalBox::Slot().AutoHeight() [ SNew(SBox)
                    .HeightOverride_Lambda([this]() { return FOptionalSize(AssetPanel.IsValid() && !AssetPanel->IsExpanded() ? 46.f : 200.f); })
                    [ SAssignNew(AssetPanel, SAgentAssetCandidatePanel, Session.ToSharedRef()) ] ]
                + SVerticalBox::Slot().AutoHeight() [ SNew(SBorder).Padding(8) [ SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight() [ SNew(SBox).HeightOverride(105) [ SAssignNew(Input, SMultiLineEditableTextBox)
                        .Text(FText::FromString(Session->DraftText))
                        .HintText(LOCTEXT("Input", "输入消息。Enter 换行，Ctrl+Enter 发送"))
                        .OnTextChanged_Lambda([this](const FText& Text) { Session->DraftText = Text.ToString(); Session->Touch(); })
                        .OnKeyDownHandler(this, &SAgentChatWindow::OnInputKeyDown) ] ]
                    + SVerticalBox::Slot().AutoHeight() [ SNew(STextBlock).Text_Lambda([this]() { return FText::FromString(SendError); }).AutoWrapText(true) ]
                    + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(0, 6, 0, 0) [ SNew(SHorizontalBox)
                        + SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0) [ SNew(SButton).Text(LOCTEXT("Stop", "停止"))
                            .IsEnabled_Lambda([this]() { return Session->Runner.State == EAgentRunState::Running; }).OnClicked(this, &SAgentChatWindow::Stop) ]
                        + SHorizontalBox::Slot().AutoWidth() [ SNew(SButton).Text(LOCTEXT("Send", "发送"))
                            .IsEnabled_Lambda([this]() { return Session->Runner.State != EAgentRunState::Running && !Session->DraftText.TrimStartAndEnd().IsEmpty(); })
                            .OnClicked(this, &SAgentChatWindow::Send) ] ]
                ] ]
            ]
            + SSplitter::Slot().Value(0.30f).MinSize(250) [ SAssignNew(ExecutionPanel, SAgentExecutionPanel, Session.ToSharedRef()) ]
        ]
        + SVerticalBox::Slot().AutoHeight() [ SNew(SBorder).Padding(6) [ SNew(STextBlock)
            .Text_Lambda([this]() { return FText::FromString(FString::Printf(TEXT("Session: %s · 状态: %s · 模型: %s · 当前任务: %s · 候选: %d"),
                *Session->SessionId.ToString(EGuidFormats::Digits).Left(8),
                Session->Runner.State == EAgentRunState::Running ? TEXT("运行中")
                    : Session->Runner.State == EAgentRunState::Completed ? TEXT("已完成")
                    : Session->Runner.State == EAgentRunState::Failed ? TEXT("失败")
                    : Session->Runner.State == EAgentRunState::Cancelled ? TEXT("已停止") : TEXT("空闲"),
                *Session->ModelOptions.Model,
                Session->Runner.CurrentRunId.IsValid() ? *Session->Runner.CurrentRunId.ToString(EGuidFormats::Digits).Left(8) : TEXT("无"),
                Session->Candidates.Num())); }) ] ]
    ];
}

void SAgentChatWindow::FocusHistory()
{
    if (HistoryPanel) { HistoryPanel->FocusList(); }
}

FReply SAgentChatWindow::Send()
{
    SendError.Empty();
    if (!Session->Send(SendError)) { AssetPanel->Refresh(); return FReply::Handled(); }
    Manager->UpdateWindowTitle(Session.ToSharedRef());
    Input->SetText(FText::GetEmpty());
    ConversationList->Refresh();
    ExecutionPanel->Refresh();
    AssetPanel->Refresh();
    HistoryPanel->Refresh();
    Manager->NotifySessionsChanged();
    return FReply::Handled();
}

void SAgentChatWindow::RefreshAfterRun()
{
    if (ConversationList) { ConversationList->Refresh(); }
    if (ExecutionPanel) { ExecutionPanel->Refresh(); }
    if (HistoryPanel) { HistoryPanel->Refresh(); }
}

FReply SAgentChatWindow::Stop()
{
    Session->CancelRun();
    ExecutionPanel->Refresh();
    return FReply::Handled();
}

FReply SAgentChatWindow::OnInputKeyDown(const FGeometry&, const FKeyEvent& Event)
{
    if (Event.IsControlDown() && Event.GetKey() == EKeys::Enter) { return Send(); }
    return FReply::Unhandled();
}

#undef LOCTEXT_NAMESPACE
