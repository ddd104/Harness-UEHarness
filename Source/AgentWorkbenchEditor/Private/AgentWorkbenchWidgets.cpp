#include "AgentWorkbenchWidgets.h"
#include "AgentAssetContextService.h"
#include "AgentWorkbenchSettings.h"
#include "AgentWorkbenchDisplayFormatter.h"
#include "AgentExecutionTreeModel.h"
#include "AgentModelClient.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Misc/MessageDialog.h"
#include "InputCoreTypes.h"
#include "Styling/StyleColors.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SScrollBar.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/SMultiLineEditableText.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Text/SInlineEditableTextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "AgentWorkbench"

namespace
{
const FSlateRoundedBoxBrush AgentCardBrush(FLinearColor::White, 6.f);
FText ApprovalModeLabel(EAgentApprovalMode Mode)
{
    switch (Mode)
    {
    case EAgentApprovalMode::Smart: return LOCTEXT("SmartApproval", "智能批准");
    case EAgentApprovalMode::Unrestricted: return LOCTEXT("UnrestrictedApproval", "无限制修改");
    default: return LOCTEXT("AskApproval", "询问修改");
    }
}

void HighlightExecutionCardOnHover(const TSharedRef<SBorder>& Card, const FSlateColor NormalColor)
{
    const TWeakPtr<SBorder> WeakCard = Card;
    Card->SetOnMouseEnter(FNoReplyPointerEventHandler::CreateLambda([WeakCard](const FGeometry&, const FPointerEvent&)
    {
        if (const TSharedPtr<SBorder> HoverCard = WeakCard.Pin())
        { HoverCard->SetBorderBackgroundColor(TAttribute<FSlateColor>(FStyleColors::Hover)); }
    }));
    Card->SetOnMouseLeave(FSimpleNoReplyPointerEventHandler::CreateLambda([WeakCard, NormalColor](const FPointerEvent&)
    {
        if (const TSharedPtr<SBorder> HoverCard = WeakCard.Pin())
        { HoverCard->SetBorderBackgroundColor(TAttribute<FSlateColor>(NormalColor)); }
    }));
}
}

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
    VisibleMessages = FAgentWorkbenchDisplayFormatter::VisibleConversationMessages(Session->Messages);
    ChildSlot [ SNew(SBorder).Padding(8) [ SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6) [ SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
                [ SNew(STextBlock).Text(LOCTEXT("Conversation", "对话记录")) ]
            + SHorizontalBox::Slot().AutoWidth()
                [ SNew(SButton).Text(LOCTEXT("ConversationLatest", "最新"))
                    .OnClicked_Lambda([this]() { ScrollToLatest(); return FReply::Handled(); }) ] ]
        + SVerticalBox::Slot().FillHeight(1) [ SAssignNew(List, SListView<TSharedPtr<FAgentMessage>>)
            .ListItemsSource(&VisibleMessages).OnGenerateRow(this, &SAgentConversationList::MakeRow)
            .SelectionMode(ESelectionMode::None) ]
    ] ];
}

void SAgentConversationList::Refresh()
{
    TArray<TSharedPtr<FAgentMessage>> Updated =
        FAgentWorkbenchDisplayFormatter::VisibleConversationMessages(Session->Messages);
    bool bChanged = Updated.Num() != VisibleMessages.Num();
    if (!bChanged)
    {
        for (int32 Index = 0; Index < Updated.Num(); ++Index)
        {
            if (Updated[Index] != VisibleMessages[Index]) { bChanged = true; break; }
        }
    }
    if (!bChanged) { return; }
    // Slate reports the remaining scroll distance as a 0..1 proportion.
    const bool bFollowLatest = List && !List->IsUserScrolling()
        && List->GetScrollDistanceRemaining().Y <= 0.02f;
    VisibleMessages = MoveTemp(Updated);
    if (List)
    {
        List->RequestListRefresh();
        if (bFollowLatest) { List->ScrollToBottom(); }
    }
}

void SAgentConversationList::ScrollToLatest()
{
    if (List) { List->ScrollToBottom(); }
}

TSharedRef<ITableRow> SAgentConversationList::MakeRow(TSharedPtr<FAgentMessage> Item, const TSharedRef<STableViewBase>& Owner)
{
    const bool bUser = Item->Role == EAgentMessageRole::User;
    const bool bFailure = Item->Role == EAgentMessageRole::Error;
    const FString Text = bFailure && Item->Text.TrimStartAndEnd().IsEmpty()
        ? TEXT("任务执行失败，未提供具体原因。") : Item->Text;
    TSharedRef<SHorizontalBox> Aligned = SNew(SHorizontalBox);
    if (bUser) { Aligned->AddSlot().FillWidth(1) [ SNew(SSpacer) ]; }
    Aligned->AddSlot().FillWidth(4) [ SNew(SBorder)
        .BorderImage(&AgentCardBrush)
        .BorderBackgroundColor(bUser ? FStyleColors::SelectInactive : FStyleColors::Panel)
        .Padding(10) [ SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 5) [ SNew(SHorizontalBox)
                + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center) [ SNew(STextBlock)
                    .Text(bUser ? LOCTEXT("UserMessageLabel", "我")
                        : bFailure ? LOCTEXT("FailureMessageLabel", "任务失败")
                        : LOCTEXT("AgentMessageLabel", "Agent"))
                    .ColorAndOpacity(bUser ? FStyleColors::AccentBlue
                        : bFailure ? FStyleColors::Error : FStyleColors::Foreground) ]
                + SHorizontalBox::Slot().AutoWidth() [ SNew(SButton)
                    .Text(LOCTEXT("CopyConversationMessage", "复制"))
                    .ToolTipText(LOCTEXT("CopyConversationMessageHint", "复制整条消息"))
                    .OnClicked_Lambda([Text]()
                    { FPlatformApplicationMisc::ClipboardCopy(*Text); return FReply::Handled(); }) ] ]
            + SVerticalBox::Slot().AutoHeight() [ SNew(SMultiLineEditableText)
                .Text(FText::FromString(Text)).IsReadOnly(true).AutoWrapText(true)
                .AllowContextMenu(true).ClearTextSelectionOnFocusLoss(false) ] ] ];
    if (!bUser) { Aligned->AddSlot().FillWidth(1) [ SNew(SSpacer) ]; }
    return SNew(STableRow<TSharedPtr<FAgentMessage>>, Owner).Padding(FMargin(3, 6)) [ Aligned ];
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
    Roots = FAgentExecutionTreeModel::Build(*Session);
    LastEventCount = Session->Events.Num();
    LastEventSequence = Session->Events.IsEmpty() || !Session->Events.Last()
        ? INDEX_NONE : Session->Events.Last()->Sequence;
    for (const TSharedPtr<FAgentExecutionNode>& Root : Roots)
    {
        if (!Root) { continue; }
        KnownRunKeys.Add(NodeKey(*Root));
        for (const TSharedPtr<FAgentExecutionNode>& Step : Root->Children)
        { if (Step) { KnownStepKeys.Add(NodeKey(*Step)); } }
    }
    if (!Roots.IsEmpty() && Roots.Last()) { ExpandedNodeKeys.Add(NodeKey(*Roots.Last())); }
    // Keep the list width stable as details make the vertical scrollbar necessary.
    TSharedRef<SScrollBar> ExecutionScrollBar = SNew(SScrollBar)
        .Orientation(Orient_Vertical)
        .ScrollbarDisabledVisibility(EVisibility::Hidden);
    ChildSlot [ SNew(SBorder).Padding(8) [ SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 6) [ SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
                [ SNew(STextBlock).Text(LOCTEXT("Execution", "任务执行详情")) ]
            + SHorizontalBox::Slot().AutoWidth() [ SNew(SButton)
                .Text(LOCTEXT("ExecutionLatest", "最新"))
                .OnClicked_Lambda([this]()
                { ScrollToLatest(); return FReply::Handled(); }) ] ]
        + SVerticalBox::Slot().FillHeight(1) [ SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1) [ SAssignNew(Tree, STreeView<TSharedPtr<FAgentExecutionNode>>)
                .TreeItemsSource(&Roots).OnGenerateRow(this, &SAgentExecutionPanel::MakeRow)
                .OnGetChildren(this, &SAgentExecutionPanel::GetNodeChildren)
                .OnExpansionChanged(this, &SAgentExecutionPanel::OnExpansionChanged)
                // STableRow otherwise toggles expansion on double-click before the card's mouse-up.
                .OnMouseButtonDoubleClick_Lambda([](TSharedPtr<FAgentExecutionNode>) {})
                .ExternalScrollbar(ExecutionScrollBar)
                .SelectionMode(ESelectionMode::None) ]
            + SHorizontalBox::Slot().AutoWidth() [ SNew(SBox).WidthOverride(16.f) [ ExecutionScrollBar ] ] ]
    ] ];
    if (!Roots.IsEmpty() && Roots.Last()) { Tree->SetItemExpansion(Roots.Last(), true); }
}

FString SAgentExecutionPanel::NodeKey(const FAgentExecutionNode& Node) const
{
    const TCHAR* Prefix = Node.Kind == EAgentExecutionNodeKind::Run ? TEXT("run")
        : Node.Kind == EAgentExecutionNodeKind::Step ? TEXT("step") : TEXT("detail");
    return FString::Printf(TEXT("%s:%s:%d"), Prefix,
        *Node.RunId.ToString(EGuidFormats::Digits), Node.Kind == EAgentExecutionNodeKind::Run ? 0 : Node.Sequence);
}

void SAgentExecutionPanel::GetNodeChildren(TSharedPtr<FAgentExecutionNode> Item,
    TArray<TSharedPtr<FAgentExecutionNode>>& OutChildren) const
{
    if (Item) { OutChildren.Append(Item->Children); }
}

void SAgentExecutionPanel::OnExpansionChanged(TSharedPtr<FAgentExecutionNode> Item, bool bExpanded)
{
    if (!Item) { return; }
    const FString Key = NodeKey(*Item);
    if (bExpanded) { ExpandedNodeKeys.Add(Key); }
    else { ExpandedNodeKeys.Remove(Key); }
}

void SAgentExecutionPanel::Refresh()
{
    const int32 EventCount = Session->Events.Num();
    const int32 LastSequence = EventCount > 0 && Session->Events.Last()
        ? Session->Events.Last()->Sequence : INDEX_NONE;
    if (EventCount == LastEventCount && LastSequence == LastEventSequence) { return; }
    const bool bFollowLatest = Tree && !Tree->IsUserScrolling()
        && Tree->GetScrollDistanceRemaining().Y <= 0.02f;
    const float PreviousOffset = Tree ? Tree->GetScrollOffset() : 0.f;
    Roots = FAgentExecutionTreeModel::Build(*Session);
    LastEventCount = EventCount;
    LastEventSequence = LastSequence;
    if (!Tree) { return; }
    Tree->RequestTreeRefresh();
    for (const TSharedPtr<FAgentExecutionNode>& Root : Roots)
    {
        if (!Root) { continue; }
        const FString RootKey = NodeKey(*Root);
        if (!KnownRunKeys.Contains(RootKey))
        {
            KnownRunKeys.Add(RootKey);
            ExpandedNodeKeys.Add(RootKey);
        }
        Tree->SetItemExpansion(Root, ExpandedNodeKeys.Contains(RootKey));
        for (const TSharedPtr<FAgentExecutionNode>& Step : Root->Children)
        {
            if (!Step) { continue; }
            const FString StepKey = NodeKey(*Step);
            if (!KnownStepKeys.Contains(StepKey))
            {
                KnownStepKeys.Add(StepKey);
                if (Step->Event && !Step->Children.IsEmpty()
                    && (Step->Event->Type == EAgentEventType::ModelRequestStarted
                        || Step->Event->Type == EAgentEventType::ModelRequestCompleted))
                { ExpandedNodeKeys.Add(StepKey); }
            }
            if (ExpandedNodeKeys.Contains(StepKey))
            { Tree->SetItemExpansion(Step, true); }
        }
    }
    if (bFollowLatest) { ScrollToLatest(); }
    else { Tree->SetScrollOffset(PreviousOffset); }
}

void SAgentExecutionPanel::ScrollToLatest()
{
    if (!Tree || Roots.IsEmpty() || !Roots.Last()) { return; }
    TSharedPtr<FAgentExecutionNode> Latest = Roots.Last();
    if (ExpandedNodeKeys.Contains(NodeKey(*Latest)) && !Latest->Children.IsEmpty())
    {
        Latest = Latest->Children.Last();
        if (ExpandedNodeKeys.Contains(NodeKey(*Latest)) && !Latest->Children.IsEmpty())
        { Latest = Latest->Children.Last(); }
    }
    // RequestScrollIntoView resolves after STreeView rebuilds its visible rows on Tick.
    Tree->RequestScrollIntoView(Latest);
}

TSharedRef<ITableRow> SAgentExecutionPanel::MakeRow(TSharedPtr<FAgentExecutionNode> Item,
    const TSharedRef<STableViewBase>& Owner)
{
    TSharedRef<STableRow<TSharedPtr<FAgentExecutionNode>>> Row =
        SNew(STableRow<TSharedPtr<FAgentExecutionNode>>, Owner).Padding(FMargin(2, 4));
    // STreeView adds its own expander arrow; the card itself now controls expansion.
    Row->SetExpanderArrowVisibility(EVisibility::Collapsed);
    if (Item->Kind == EAgentExecutionNodeKind::Detail)
    {
        FString Detail = Item->DisplayDetail;
        if (Item->Event)
        {
            FAgentEvent DisplayEvent = *Item->Event;
            DisplayEvent.Detail = Item->DisplayDetail;
            Detail = FAgentWorkbenchDisplayFormatter::FormatEventDetail(DisplayEvent);
        }
        Row->SetContent(SNew(SBox).Padding(FMargin(28, 0, 0, 0)) [ SNew(SBorder)
                .BorderImage(&AgentCardBrush)
                .BorderBackgroundColor(FStyleColors::Header).Padding(10)
                [ SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
                        [ SNew(SButton).Text(LOCTEXT("CopyExecutionDetail", "复制详情"))
                            .OnClicked_Lambda([Detail]()
                            { FPlatformApplicationMisc::ClipboardCopy(*Detail); return FReply::Handled(); }) ]
                    + SVerticalBox::Slot().AutoHeight() [ SNew(SBox).MaxDesiredHeight(400)
                        [ SNew(SScrollBox)
                            + SScrollBox::Slot() [ SNew(SMultiLineEditableText)
                                .Text(FText::FromString(Detail)).IsReadOnly(true).AutoWrapText(true)
                                .AllowContextMenu(true).ClearTextSelectionOnFocusLoss(false) ] ] ] ] ]);
        return Row;
    }
    if (Item->Kind == EAgentExecutionNodeKind::Run)
    {
        FString Status = TEXT("未完成");
        FSlateColor StatusColor = FStyleColors::Secondary;
        switch (FAgentExecutionTreeModel::StatusFor(*Session, *Item))
        {
        case EAgentExecutionRunStatus::Completed:
            Status = TEXT("已完成"); StatusColor = FStyleColors::Success; break;
        case EAgentExecutionRunStatus::Failed:
            Status = TEXT("失败"); StatusColor = FStyleColors::Error; break;
        case EAgentExecutionRunStatus::Cancelled:
            Status = TEXT("已停止"); StatusColor = FStyleColors::Warning; break;
        case EAgentExecutionRunStatus::Running:
            Status = TEXT("进行中"); StatusColor = FStyleColors::AccentBlue; break;
        case EAgentExecutionRunStatus::Interrupted:
            Status = TEXT("已中断"); StatusColor = FStyleColors::Warning; break;
        default: break;
        }
        const FString Title = Item->Title;
        TSharedPtr<SBorder> RunCard;
        Row->SetContent(SAssignNew(RunCard, SBorder)
            .BorderImage(&AgentCardBrush)
            .BorderBackgroundColor(FStyleColors::Panel).Padding(FMargin(6, 8))
            .ToolTipText(LOCTEXT("ToggleExecutionRunHint", "点击查看或收起本轮执行步骤"))
            .OnMouseButtonUp_Lambda([this, Item](const FGeometry&, const FPointerEvent& MouseEvent)
            {
                if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton || !Tree
                    || Item->Children.IsEmpty()) { return FReply::Unhandled(); }
                Tree->SetItemExpansion(Item, !Tree->IsItemExpanded(Item));
                return FReply::Handled();
            })
            [ SNew(SHorizontalBox)
                + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
                    [ SNew(STextBlock).Text(FText::FromString(Title)).AutoWrapText(true) ]
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(5, 0)
                    [ SNew(STextBlock).Text(FText::FromString(Status)).ColorAndOpacity(StatusColor) ]
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(5, 0, 0, 0)
                    [ SNew(SButton).Text(LOCTEXT("CopyExecutionRun", "复制"))
                        .ToolTipText(LOCTEXT("CopyExecutionRunHint", "复制本轮标题"))
                        .OnClicked_Lambda([Title]()
                        { FPlatformApplicationMisc::ClipboardCopy(*Title); return FReply::Handled(); }) ] ]);
        HighlightExecutionCardOnHover(RunCard.ToSharedRef(), FStyleColors::Panel);
        return Row;
    }
    const FString Summary = Item->Title;
    TSharedPtr<SBorder> StepCard;
    Row->SetContent(SNew(SBox).Padding(FMargin(14, 0, 0, 0)) [ SAssignNew(StepCard, SBorder)
        .BorderImage(&AgentCardBrush)
        .BorderBackgroundColor(FStyleColors::Recessed).Padding(FMargin(5, 6))
        .ToolTipText(Item->Children.IsEmpty()
            ? FText::GetEmpty() : LOCTEXT("ToggleExecutionStepHint", "点击查看或收起步骤详情"))
        .OnMouseButtonUp_Lambda([this, Item](const FGeometry&, const FPointerEvent& MouseEvent)
        {
            if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton || !Tree
                || Item->Children.IsEmpty()) { return FReply::Unhandled(); }
            Tree->SetItemExpansion(Item, !Tree->IsItemExpanded(Item));
            return FReply::Handled();
        })
        [ SNew(SHorizontalBox)
            + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
                [ SNew(STextBlock).Text(FText::FromString(Summary)).AutoWrapText(true) ]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0)
                [ SNew(STextBlock)
                    .Visibility(Item->Children.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible)
                    .Text_Lambda([this, Item]()
                    { return Tree && Tree->IsItemExpanded(Item)
                        ? LOCTEXT("HideExecutionDetail", "收起详情")
                        : LOCTEXT("ShowExecutionDetail", "查看详情"); })
                    .ColorAndOpacity(FStyleColors::Secondary) ]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                [ SNew(SButton).Text(LOCTEXT("CopyExecutionStep", "复制"))
                    .OnClicked_Lambda([Summary]()
                    { FPlatformApplicationMisc::ClipboardCopy(*Summary); return FReply::Handled(); }) ] ] ]);
    HighlightExecutionCardOnHover(StepCard.ToSharedRef(), FStyleColors::Recessed);
    return Row;
}

void SAgentChatWindow::Construct(const FArguments& Args)
{
    Session = Args._Session;
    Manager = Args._Manager;
    check(Session && Manager);
    ApprovalChoices = {MakeShared<EAgentApprovalMode>(EAgentApprovalMode::Ask),
        MakeShared<EAgentApprovalMode>(EAgentApprovalMode::Smart),
        MakeShared<EAgentApprovalMode>(EAgentApprovalMode::Unrestricted)};
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
                    + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Fill).Padding(0, 6, 0, 0) [ SNew(SHorizontalBox)
                        + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 8, 0)
                            [ SNew(SComboBox<TSharedPtr<EAgentApprovalMode>>).OptionsSource(&ApprovalChoices)
                                .ToolTipText(LOCTEXT("ApprovalModeTooltip", "询问修改：逐次确认编辑和打开资产；智能批准：自动允许已明确列入只读名单的工具，其余逐次确认；无限制修改：自动允许已注册工具，仍执行参数、路径和超时校验。发送时固定本轮权限。"))
                                .OnGenerateWidget_Lambda([](TSharedPtr<EAgentApprovalMode> Mode)
                                { return SNew(STextBlock).Text(ApprovalModeLabel(*Mode)); })
                                .OnSelectionChanged_Lambda([this](TSharedPtr<EAgentApprovalMode> Mode, ESelectInfo::Type)
                                { if (Mode) { Session->ApprovalMode = *Mode; Session->Touch(); } })
                                [ SNew(STextBlock).Text_Lambda([this]()
                                { return ApprovalModeLabel(Session->ApprovalMode); }) ] ]
                        + SHorizontalBox::Slot().FillWidth(1) [ SNew(SSpacer) ]
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
    ConversationList->ScrollToLatest();
    ExecutionPanel->Refresh();
    ExecutionPanel->ScrollToLatest();
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
