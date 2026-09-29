#pragma once

#include "CoreMinimal.h"
#include "AgentWorkbenchSettings.h"
#include "Containers/Ticker.h"
#include <atomic>

class SWindow;
class IHttpRequest;
class FAgentToolWorkflow;

enum class EAgentMessageRole : uint8 { User, Assistant, Tool, Error };
enum class EAgentRunState : uint8 { Idle, Running, Completed, Cancelled, Failed, Interrupted };
enum class EAgentRunPhase : uint8 { None, RequestingModel, ProcessingTools };
enum class EAgentEventType : uint8 { RunStarted, ModelRequestStarted, ModelRequestCompleted, RunCompleted, RunCancelled, ToolCallStarted, ToolCallCompleted, RunFailed };
enum class EAgentCandidateValidation : uint8 { Unknown, Valid, Missing, Unsupported };
enum class EAgentApprovalMode : uint8 { Ask, Smart, Unrestricted };

struct FAgentToolCall
{
    FString Id;
    FString Name;
    FString ArgumentsJson;
    FString ThoughtSignature;
};

struct FAgentCandidate
{
    FGuid CandidateId = FGuid::NewGuid();
    FString DisplayName;
    FString ObjectPath;
    FString PackageName;
    FString AssetClassPath;
    bool bIncluded = true;
    EAgentCandidateValidation ValidationStatus = EAgentCandidateValidation::Unknown;
    FDateTime AddedAt = FDateTime::UtcNow();
};

struct FAgentMessage
{
    FGuid MessageId = FGuid::NewGuid();
    EAgentMessageRole Role = EAgentMessageRole::User;
    FString Text;
    FGuid RunId;
    TArray<FAgentCandidate> IncludedAssets;
    TArray<FAgentToolCall> ToolCalls;
    FString ToolCallId;
    FString ToolName;
};

struct FAgentEvent
{
    FGuid RunId;
    int32 Sequence = 0;
    EAgentEventType Type = EAgentEventType::RunStarted;
    FDateTime Time = FDateTime::UtcNow();
    FString Summary;
    FString Detail;
};

struct FAgentModelOptions
{
    FString Model;
    int32 MaxOutputTokens = 2048;
};

struct FAgentConversationTurn
{
    EAgentMessageRole Role = EAgentMessageRole::User;
    FString Text;
    TArray<FAgentCandidate> IncludedAssets;
    TArray<FAgentToolCall> ToolCalls;
    FString ToolCallId;
    FString ToolName;
};

struct FAgentRunInputSnapshot
{
    FGuid SessionId;
    FGuid RunId;
    FString UserInput;
    FAgentModelOptions ModelOptions;
    FString Provider;
    EAgentWorkbenchProvider ProviderType = EAgentWorkbenchProvider::DeepSeek;
    EAgentApprovalMode ApprovalMode = EAgentApprovalMode::Ask;
    TArray<FAgentConversationTurn> Conversation;
    TArray<FAgentCandidate> IncludedAssets;
    TArray<FString> AllowedToolNames;
    // Captured at Run start so a configuration reload cannot rebind an allowed name.
    TMap<FString, FString> AllowedToolHandlerIds;
    bool bToolListFrozen = false;
    int32 RequestTimeoutSeconds = 120;
    int32 ToolTimeoutSeconds = 60;
    int32 RunTimeoutSeconds = 900;
    int32 MaxToolSteps = 16;
    FDateTime CreatedAt = FDateTime::UtcNow();
};

class FAgentRunner
{
public:
    EAgentRunState State = EAgentRunState::Idle;
    EAgentRunPhase Phase = EAgentRunPhase::None;
    FGuid CurrentRunId;
    FGuid CurrentRequestId;
    FString CurrentToolCallId;
    TSharedPtr<const FAgentRunInputSnapshot> LastSnapshot;
    TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> ActiveRequest;
    TSharedPtr<FAgentToolWorkflow> ToolWorkflow;
    TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> CancellationToken;
    TArray<FAgentConversationTurn> Conversation;
    TArray<FAgentToolCall> PendingToolCalls;
    int32 NextToolCallIndex = 0;
    int32 ToolSteps = 0;
    double RunDeadlineSeconds = 0.0;
    FTSTicker::FDelegateHandle RunTimeoutHandle;
    void Cancel();
};

class FAgentSession : public TSharedFromThis<FAgentSession>
{
public:
    FGuid SessionId = FGuid::NewGuid();
    FString Title = TEXT("新对话");
    bool bTitleManuallySet = false;
    FDateTime CreatedAt = FDateTime::UtcNow();
    FDateTime UpdatedAt = CreatedAt;
    FAgentModelOptions ModelOptions;
    EAgentApprovalMode ApprovalMode = EAgentApprovalMode::Ask;
    FString DraftText;
    TArray<TSharedPtr<FAgentMessage>> Messages;
    TArray<TSharedPtr<FAgentEvent>> Events;
    TArray<TSharedPtr<FAgentCandidate>> Candidates;
    TArray<FAgentRunInputSnapshot> RunHistory;
    FAgentRunner Runner;
    int32 NextEventSequence = 1;
    TFunction<void(const FAgentSession&)> OnChanged;
    FSimpleMulticastDelegate OnUiChanged;

    void Touch();
    bool HasStartedRun() const { return !RunHistory.IsEmpty(); }

    void AddEvent(EAgentEventType Type, const FGuid& RunId, const FString& Summary, const FString& Detail = FString());
    bool BeginRun(FString& OutError);
    bool Send(FString& OutError);
    void CompleteRun(const FGuid& RunId, const FString& Reply);
    void FailRun(const FGuid& RunId, const FString& Error);
    void OnModelResponse(const FGuid& RunId, const FGuid& RequestId,
        const FString& ReplyText, const TArray<FAgentToolCall>& ToolCalls);
    void ExecuteNextTool(const FGuid& RunId);
    void OnToolResult(const FGuid& RunId, const FString& ToolCallId, const FString& ToolName,
        const FString& ResultText);
    void CancelRun();
};

class FAgentSessionManager : public TSharedFromThis<FAgentSessionManager>
{
public:
    explicit FAgentSessionManager(const FString& InStorageDirectory = FString());
    FSimpleMulticastDelegate OnSessionsChanged;
    TSharedRef<FAgentSession> CreateSession();
    TSharedRef<FAgentSession> ForkSession(const TSharedRef<FAgentSession>& Source, bool bOpenWindow = true);
    void OpenSession(const TSharedRef<FAgentSession>& Session);
    bool SwitchSessionInWindow(const FGuid& CurrentSessionId, const TSharedRef<FAgentSession>& Target);
    void RequestSwitchSession(const FGuid& CurrentSessionId, const TSharedRef<FAgentSession>& Target);
    bool DeleteSession(const TSharedRef<FAgentSession>& Target);
    void RequestDeleteSession(const TSharedRef<FAgentSession>& Target);
    bool RenameSession(const TSharedRef<FAgentSession>& Target, const FString& NewTitle);
    TSharedPtr<SWindow> FindWindowForSession(const FGuid& SessionId) const;
    void UpdateWindowTitle(const TSharedRef<FAgentSession>& Session);
    void NotifySessionsChanged() { OnSessionsChanged.Broadcast(); }
    void Shutdown();
    int32 GetOpenWindowCount() const;
    const TArray<TSharedPtr<FAgentSession>>& GetSessions() const { return Sessions; }
    TArray<TSharedPtr<FAgentSession>> GetHistorySessions() const;

private:
    void AttachPersistence(const TSharedRef<FAgentSession>& Session);
    FString StorageDirectory;
    TArray<TSharedPtr<FAgentSession>> Sessions;
    TMap<FGuid, TWeakPtr<SWindow>> Windows;
};
