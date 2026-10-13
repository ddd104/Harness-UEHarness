#pragma once

#include "CoreMinimal.h"
#include "AgentWorkbenchSession.h"
#include "HAL/CriticalSection.h"
#include "MCPToolTypes.h"
#include <atomic>

class IHttpRequest;

/** One memory: a single .md file's parsed content. */
struct FAgentMemoryRecord
{
    FString Name;
    FString Type;        // user | feedback | project | reference
    FString Description; // one-line summary shown in the index
    FString Body;        // full content
};

/**
 * Memory (s09): cross-session reusable knowledge stored as one Markdown file per memory
 * under <Project>/Memory/, with MEMORY.md as a derived index.
 *
 *   Storage        write/delete/rebuild-index, thread-safe (Game Thread + mutation lock)
 *   Recall         select <= MaxRecallCount via a lightweight model call, fall back to
 *                  keyword matching, then load bodies (capped total) into the system prompt
 *   Extraction     after a Run completes, the model proposes candidates with a "scope";
 *                  only persistent candidates passing ShouldStoreMemory are written
 *   Consolidation  at >= ConsolidateThreshold files, the model merges/dedupes/prunes;
 *                  snapshot + rollback on failure
 *
 * The three background model calls reuse FAgentModelClient::RunTurn and may target an
 * independent lightweight model (MemoryProvider/MemoryModel) when bUseMemoryModel is set.
 * Memory content is data, never instructions (see SPEC section 6).
 */
class FAgentMemoryStore
{
public:
    static FAgentMemoryStore& Get();

    // Meta tool names and native handler ids (memory.remember / memory.forget).
    static const TCHAR* RememberToolName();
    static const TCHAR* RememberToolHandlerId();
    static const TCHAR* ForgetToolName();
    static const TCHAR* ForgetToolHandlerId();

    // Builds the frozen config from UAgentWorkbenchSettings with safe clamping.
    static FAgentMemoryConfig ReadConfig();

    void Shutdown();

    // Overrides the memory directory for tests only; empty restores <Project>/Memory/.
    void SetTestDirectory(const FString& Directory);

    // ---- Storage (synchronous, Game Thread, serialized by a mutation lock) ----
    FString MemoryDirectory();
    bool HasMemories();
    TArray<FAgentMemoryRecord> ReadMemoryFiles();
    bool WriteMemory(const FString& Name, const FString& Type, const FString& Description,
        const FString& Body, FString& OutError);
    bool ForgetMemory(const FString& Name, FString& OutError);
    FString BuildCatalog(const TArray<FAgentMemoryRecord>& Records) const;

    // ---- Validation (shared by tools and extraction) ----
    static bool ValidateType(const FString& Type);
    // Final gate: rejects incomplete fields, temporary wording, non-persistent scope, and
    // duplicates. OutRejectReason is empty when the candidate should be stored.
    static bool ShouldStoreMemory(const FString& Name, const FString& Type,
        const FString& Description, const FString& Body, const FString& Scope,
        const TArray<FAgentMemoryRecord>& Existing, FString& OutRejectReason);

    // ---- Recall ----
    static TArray<FAgentMemoryRecord> KeywordSelect(const TArray<FAgentMemoryRecord>& Records,
        const FString& UserRequest, int32 MaxCount);
    static FString FormatRecallContext(const TArray<FAgentMemoryRecord>& Selected, int32 MaxBodyChars);
    static FString KeywordRecallContext(const FString& UserRequest, const FAgentMemoryConfig& Config);
    // Async selection via a model call; Completion always receives a final context string
    // (empty when no memory applies; keyword-selected on model failure/parse error).
    void SelectRelevantAsync(const FAgentRunInputSnapshot& Snapshot, const FString& UserRequest,
        const FAgentMemoryConfig& Config, double DeadlineSeconds,
        const TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe>& CancellationToken,
        TSharedPtr<IHttpRequest, ESPMode::ThreadSafe>& OutActiveRequest,
        TFunction<void(const FString& Context)> Completion);

    // ---- Extraction + Consolidation (post-run, fire-and-forget) ----
    void ExtractAsync(const FAgentRunInputSnapshot& Snapshot,
        const TArray<FAgentConversationTurn>& Conversation, const FString& FinalReply,
        const FAgentMemoryConfig& Config, TFunction<void(int32 WrittenCount)> Completion);

    // ---- Pure helpers (exposed for tests) ----
    static FString MemorySlug(const FString& Name);
    static bool ParseJsonArray(const FString& Text, TArray<TSharedPtr<FJsonValue>>& OutArray);

private:
    FAgentMemoryStore() = default;
    ~FAgentMemoryStore() = default;
    FAgentMemoryStore(const FAgentMemoryStore&) = delete;
    FAgentMemoryStore& operator=(const FAgentMemoryStore&) = delete;

    FString IndexPath();

    // Locked helpers: the caller holds MutationLock.
    void ListMemoryFilesLocked(TArray<FString>& OutPaths);
    TArray<FAgentMemoryRecord> ReadMemoryFilesLocked();
    bool RebuildIndexLocked();
    void SnapshotFilesLocked(TMap<FString, FString>& OutSnapshot);
    void RestoreSnapshotLocked(const TMap<FString, FString>& InSnapshot);
    bool ApplyConsolidatedLocked(const TArray<FAgentMemoryRecord>& Consolidated);

    void PruneInFlightRequests();
    void ConsolidateAsync(const FAgentRunInputSnapshot& Snapshot, const FAgentMemoryConfig& Config,
        TFunction<void()> Completion);

    // A snapshot copy for a background memory call: optionally a different provider/model
    // and always a memory-specific system prompt and output token limit.
    static FAgentRunInputSnapshot MemoryModelSnapshot(const FAgentRunInputSnapshot& Snapshot,
        const FAgentMemoryConfig& Config, const FString& SystemPrompt, int32 MaxTokens);

    mutable FCriticalSection MutationLock;
    TArray<TSharedPtr<IHttpRequest, ESPMode::ThreadSafe>> InFlightRequests;
    bool bShuttingDown = false;
    // Empty means <Project>/Memory/; set only by tests.
    FString TestDirectory;
};
