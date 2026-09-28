#pragma once

#include "CoreMinimal.h"
#include "AgentWorkbenchSession.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class SMultiLineEditableTextBox;
class SInlineEditableTextBlock;
template<typename OptionType> class SComboBox;

class SAgentHistoryPanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SAgentHistoryPanel) {} SLATE_END_ARGS()
    void Construct(const FArguments& Args, TSharedRef<FAgentSessionManager> InManager, const FGuid& InCurrentSessionId);
    void Refresh();
    void FocusList();
private:
    TSharedRef<ITableRow> MakeRow(TSharedPtr<FAgentSession> Item, const TSharedRef<STableViewBase>& Owner);
    TSharedPtr<SWidget> MakeContextMenu();
    FReply OnHistoryKeyDown(const FGeometry& Geometry, const FKeyEvent& Event);
    void BeginRename(const TSharedRef<FAgentSession>& Item);
    void ConfirmDelete(const TSharedRef<FAgentSession>& Item);
    TSharedPtr<FAgentSessionManager> Manager;
    FGuid CurrentSessionId;
    TSharedPtr<SListView<TSharedPtr<FAgentSession>>> List;
    FString Search;
    TArray<TSharedPtr<FAgentSession>> Filtered;
    TMap<FGuid, TWeakPtr<SInlineEditableTextBlock>> RenameWidgets;
};

class SAgentModelOptionsBar : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SAgentModelOptionsBar) {} SLATE_END_ARGS()
    void Construct(const FArguments& Args, TSharedRef<FAgentSession> InSession);
private:
    void RefreshModels();
    TSharedPtr<FAgentSession> Session;
    TArray<TSharedPtr<FString>> ModelChoices;
    TSharedPtr<SComboBox<TSharedPtr<FString>>> ModelCombo;
};

class SAgentConversationList : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SAgentConversationList) {} SLATE_END_ARGS()
    void Construct(const FArguments& Args, TSharedRef<FAgentSession> InSession);
    void Refresh();
private:
    TSharedRef<ITableRow> MakeRow(TSharedPtr<FAgentMessage> Item, const TSharedRef<STableViewBase>& Owner);
    TSharedPtr<FAgentSession> Session;
    TSharedPtr<SListView<TSharedPtr<FAgentMessage>>> List;
};

class SAgentAssetCandidatePanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SAgentAssetCandidatePanel) {} SLATE_END_ARGS()
    void Construct(const FArguments& Args, TSharedRef<FAgentSession> InSession);
    void Refresh();
    bool IsExpanded() const { return bExpanded; }
private:
    FReply AddSelected();
    FReply Clear();
    FText GetFeedbackText() const;
    TSharedRef<ITableRow> MakeRow(TSharedPtr<FAgentCandidate> Item, const TSharedRef<STableViewBase>& Owner);
    TSharedPtr<FAgentSession> Session;
    TSharedPtr<SListView<TSharedPtr<FAgentCandidate>>> List;
    FString Feedback;
    bool bExpanded = true;
};

class SAgentExecutionPanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SAgentExecutionPanel) {} SLATE_END_ARGS()
    void Construct(const FArguments& Args, TSharedRef<FAgentSession> InSession);
    void Refresh();
private:
    TSharedRef<ITableRow> MakeRow(TSharedPtr<FAgentEvent> Item, const TSharedRef<STableViewBase>& Owner);
    TSharedPtr<FAgentSession> Session;
    TSharedPtr<SListView<TSharedPtr<FAgentEvent>>> List;
};

class SAgentChatWindow : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SAgentChatWindow) {}
        SLATE_ARGUMENT(TSharedPtr<FAgentSession>, Session)
        SLATE_ARGUMENT(TSharedPtr<FAgentSessionManager>, Manager)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args);
    void FocusHistory();
private:
    FReply Send();
    FReply Stop();
    FReply OnInputKeyDown(const FGeometry& Geometry, const FKeyEvent& Event);
    void RefreshAfterRun();
    TSharedPtr<FAgentSession> Session;
    TSharedPtr<FAgentSessionManager> Manager;
    TSharedPtr<SAgentHistoryPanel> HistoryPanel;
    TSharedPtr<SAgentConversationList> ConversationList;
    TSharedPtr<SAgentExecutionPanel> ExecutionPanel;
    TSharedPtr<SAgentAssetCandidatePanel> AssetPanel;
    TSharedPtr<SMultiLineEditableTextBox> Input;
    FString SendError;
};
