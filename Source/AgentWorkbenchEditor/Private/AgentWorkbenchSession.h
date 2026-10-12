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

/** Context Compact (s08) thresholds, frozen at Run start like the other limits. */
struct FAgentCompactConfig
{
    bool bEnabled = true;
    int32 ContextCharLimit = 50000;
    int32 ToolResultBudgetChars = 200000;
    int32 LargeResultCharLimit = 30000;
    int32 SnipMaxMessages = 50;
    int32 KeepRecentResults = 3;
    int32 KeepRecentMessages = 5;
    int32 MaxReactiveRetries = 1;
    int32 SummaryMaxOutputTokens = 4096;
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
    bool bSubagentToolEnabled = true;
    int32 SubagentMaxTurns = 12;
    int32 SubagentTimeoutSeconds = 300;
    int32 SubagentMaxOutputTokens = 8000;
    // Skill catalog system prompt, frozen at Run start. Empty when no skills exist.
    FString SystemPrompt;
    // Context Compact thresholds, frozen at Run start. Derived from project settings.
    FAgentCompactConfig CompactConfig;
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
    // Context Compact: true while a summary request (Step 4 or reactive) is in flight.
    bool bCompacting = false;
    // True when the model requested the compact tool in the current batch.
    bool bCompactRequested = false;
    int32 ReactiveRetries = 0;
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
    bool bPromptHookActive = false;
    int32 NextEventSequence = 1;
    TFunction<void(const FAgentSession&)> OnChanged;
    FSimpleMulticastDelegate OnUiChanged;

    void Touch();
    bool HasStartedRun() const { return !RunHistory.IsEmpty(); }

    void AddEvent(EAgentEventType Type, const FGuid& RunId, const FString& Summary, const FString& Detail = FString());
    bool BeginRun(FString& OutError);
    bool Send(FString& OutError);
    void CompleteRun(const FGuid& RunId, const FString& Reply);
    void FailRun(const FGuid& RunId, const FString& Error, bool bTimedOut = false);
    void OnModelResponse(const FGuid& RunId, const FGuid& RequestId,
        const FString& ReplyText, const TArray<FAgentToolCall>& ToolCalls,
        const FString& ResponseText);
    void ExecuteNextTool(const FGuid& RunId);
    void OnToolResult(const FGuid& RunId, const FString& ToolCallId, const FString& ToolName,
        const FString& ResultText);
    void CancelRun();

    // Context Compact: prepare + (optionally) summarize + start the next model request.
    void RequestNextModelTurn(const FGuid& RunId);
    void BeginCompactHistory(const FGuid& RunId);
    void OnCompactHistorySummary(const FGuid& RunId, bool bOk, const FString& Summary,
        const FString& Transcript, const FString& ActiveRequest);
    void BeginReactiveCompact(const FGuid& RunId);
    void OnReactiveCompactSummary(const FGuid& RunId, bool bOk, const FString& Summary,
        int32 TailStart, const FString& Transcript, const FString& ActiveRequest);
    void OnModelTooLong(const FGuid& RunId);
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
