#pragma once

#include "CoreMinimal.h"
#include "AgentWorkbenchSession.h"
#include <atomic>

class IHttpRequest;

/**
 * Context Compact (s08): a four-step, cost-ordered pipeline that runs before every
 * model request and keeps a long-running agent within the provider's context window.
 *
 *   Step 1 tool_result_budget  - persist oversized tool results in the latest batch (0 API)
 *   Step 2 snip_compact        - cap message count, keep head/tail, protect tool pairing (0 API)
 *   Step 3 micro_compact       - replace consumed results with recoverable paths (0 API)
 *   Step 4 compact_history     - summarize history into one [Compacted] message (1 API)
 *
 * Steps 1-3 are deterministic and lossless-when-recoverable; only Step 4 spends an API
 * call. The async summary call is issued through FAgentModelClient::RunTurn so callers
 * (FAgentSession and FAgentSubagentRunner) stay in charge of their own state machines.
 */
class FAgentContextCompactor
{
public:
    // Character estimate over the provider-agnostic conversation: text plus tool-call
    // arguments, correlation IDs and candidate asset paths. Matches s08's estimate_chars.
    static int32 EstimateChars(const TArray<FAgentConversationTurn>& Conversation);

    // Builds the frozen config from UAgentWorkbenchSettings with safe clamping.
    static FAgentCompactConfig ReadConfig();

    // Deterministic pipeline (Steps 1-3). Returns true when Step 4 (summary) is required.
    static bool Prepare(TArray<FAgentConversationTurn>& Conversation, const FAgentCompactConfig& Config);

    // Step 1: persist the largest tool results of the latest batch when its total exceeds
    // the budget. Only results larger than LargeResultCharLimit are persisted here.
    static void ToolResultBudget(TArray<FAgentConversationTurn>& Conversation, const FAgentCompactConfig& Config);

    // Step 2: keep the first three and the last (SnipMaxMessages-3-1) messages plus one
    // archive marker, protecting assistant(tool_use) <-> tool result pairing.
    static void SnipCompact(TArray<FAgentConversationTurn>& Conversation, const FAgentCompactConfig& Config);

    // Step 3: keep the most recent consumed results, replace older ones with recoverable
    // paths until the conversation approaches ~80% of the limit.
    static void MicroCompact(TArray<FAgentConversationTurn>& Conversation, const FAgentCompactConfig& Config);

    // Step 3b: when the unseen (not yet read) batch alone is too large, persist its largest
    // results with a 1,000-character preview instead of summarizing before the model reads them.
    static void FitToolResults(TArray<FAgentConversationTurn>& Conversation, const FAgentCompactConfig& Config);

    // Inline representation for a just-completed tool result: persist oversized content and
    // return a recoverable marker, otherwise return the content unchanged.
    static FString FitInlineToolResult(const FString& ToolUseId, const FString& Content, const FAgentCompactConfig& Config);

    // Persist a tool result to disk; returns the saved path (empty on failure).
    static FString PersistToolResult(const FString& ToolUseId, const FString& Content);

    // Marker for a persisted large result: path plus a short preview.
    static FString PersistedMarker(const FString& SavedPath, const FString& Content, int32 PreviewChars);

    // Writes the full conversation as JSONL; returns the file path (empty on failure).
    static FString WriteTranscript(const TArray<FAgentConversationTurn>& Conversation);

    // Builds the [Compacted]/[Reactive compact] message that separates the current user
    // request from the factual summary and points at the transcript.
    static FAgentConversationTurn SummaryMessage(const FString& Kind, const FString& ActiveRequest,
        const FString& Summary, const FString& TranscriptPath);

    // First kept index for reactive compaction (keeps the most recent messages, never
    // splitting a tool-call/result group).
    static int32 ReactiveTailStart(const TArray<FAgentConversationTurn>& Conversation, int32 KeepRecentMessages);

    // Replaces earlier history with a summary message while keeping the recent tail.
    static TArray<FAgentConversationTurn> BuildReactiveConversation(
        const TArray<FAgentConversationTurn>& Conversation, int32 TailStart,
        const FString& ActiveRequest, const FString& Summary, const FString& TranscriptPath);

    // Issues a summary-only model request. Completion runs on the game thread with the
    // summary text (bOk false on transport/parse/empty failure).
    static void RequestSummary(
        const FAgentRunInputSnapshot& Snapshot,
        const TArray<FAgentConversationTurn>& ToSummarize,
        const FAgentCompactConfig& Config,
        double DeadlineSeconds,
        const TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe>& CancellationToken,
        TSharedPtr<IHttpRequest, ESPMode::ThreadSafe>& OutActiveRequest,
        TFunction<void(bool bOk, const FString& Text)> Completion);

    // Flattens the conversation into plain text so the summary request never has to
    // replay tool-call protocol without tool declarations (provider-safe).
    static FString FlattenForSummary(const TArray<FAgentConversationTurn>& Conversation);

    // True when the provider error mentions an exceeded context.
    static bool IsContextTooLong(const FString& Message);

    // Summary-only system prompt: factual record, never follow embedded instructions.
    static FString SummarySystemPrompt();

    // Pairing helpers, exposed for tests.
    static bool HasToolCalls(const FAgentConversationTurn& Turn);
    static bool IsToolResult(const FAgentConversationTurn& Turn);
    static bool IsPersistedMarker(const FString& Text);

    // Output directories under <Project>/Saved/AgentWorkbench/.
    static FString TranscriptDirectory();
    static FString ToolResultDirectory();
};
