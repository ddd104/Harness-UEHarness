#include "AgentHookRegistry.h"

#include "Async/Async.h"
#include "Misc/AssertionMacros.h"
#include <atomic>

namespace
{
template<typename THandler>
struct THookEntry
{
    FAgentHookHandle Handle;
    FName Name;
    int32 Priority;
    THandler Handler;

    THookEntry(FAgentHookHandle InHandle, FName InName, int32 InPriority, THandler InHandler)
        : Handle(InHandle), Name(InName), Priority(InPriority), Handler(MoveTemp(InHandler)) {}
};

template<typename THandler>
using THookEntries = TArray<TSharedPtr<THookEntry<THandler>>>;

template<typename THandler>
FAgentHookHandle AddEntry(THookEntries<THandler>& Entries, EAgentHookPoint Point,
    uint64& NextId, FName Name, int32 Priority, THandler Handler, bool bExclusive = false)
{
    check(IsInGameThread());
    if (Name.IsNone() || !Handler || (bExclusive && !Entries.IsEmpty()))
    {
        return {};
    }
    for (const TSharedPtr<THookEntry<THandler>>& Entry : Entries)
    {
        if (Entry->Name == Name) { return {}; }
    }
    const FAgentHookHandle Handle{Point, NextId++};
    Entries.Add(MakeShared<THookEntry<THandler>>(Handle, Name, Priority, MoveTemp(Handler)));
    Entries.Sort([](const TSharedPtr<THookEntry<THandler>>& A,
        const TSharedPtr<THookEntry<THandler>>& B)
    {
        return A->Priority != B->Priority ? A->Priority < B->Priority
            : A->Handle.Id < B->Handle.Id;
    });
    return Handle;
}

template<typename THandler>
bool RemoveEntry(THookEntries<THandler>& Entries, uint64 Id)
{
    return Entries.RemoveAll([Id](const TSharedPtr<THookEntry<THandler>>& Entry)
        { return Entry->Handle.Id == Id; }) != 0;
}

template<typename THandler>
TSharedPtr<THookEntry<THandler>> FindEntry(const THookEntries<THandler>& Entries, uint64 Id)
{
    for (const TSharedPtr<THookEntry<THandler>>& Entry : Entries)
    {
        if (Entry->Handle.Id == Id) { return Entry; }
    }
    return nullptr;
}

template<typename THandler>
TArray<uint64> SnapshotIds(const THookEntries<THandler>& Entries)
{
    TArray<uint64> Ids;
    Ids.Reserve(Entries.Num());
    for (const TSharedPtr<THookEntry<THandler>>& Entry : Entries)
    {
        Ids.Add(Entry->Handle.Id);
    }
    return Ids;
}

struct FToolBeforeState
{
    FAgentHookToolContext Context;
    TArray<uint64> HandlerIds;
    FAgentHookToolBeforeCompletion Complete;
    TFunction<void()> Advance;
    int32 Index = 0;
    bool bFinished = false;
    bool bNeedsVisibleEditorFrame = false;
};

struct FToolApprovalState
{
    FGuid InvocationId;
    FAgentHookToolApprovalCompletion Complete;
    bool bFinished = false;
};

template<typename TState>
using TPendingToolStates = TMap<FGuid,
    TArray<TSharedPtr<TState, ESPMode::ThreadSafe>>>;

template<typename TState>
void RemovePendingState(TPendingToolStates<TState>& Pending, const FGuid& InvocationId,
    const TState* State)
{
    TArray<TSharedPtr<TState, ESPMode::ThreadSafe>>* States = Pending.Find(InvocationId);
    if (!States) { return; }
    States->RemoveAll([State](const TSharedPtr<TState, ESPMode::ThreadSafe>& Candidate)
        { return Candidate.Get() == State; });
    if (States->IsEmpty()) { Pending.Remove(InvocationId); }
}
}

struct FAgentHookRegistry::FImpl
{
    uint64 NextId = 1;
    // Async completions must not enter the registry after module shutdown.
    TSharedPtr<bool, ESPMode::ThreadSafe> Lifetime = MakeShared<bool, ESPMode::ThreadSafe>(true);
    THookEntries<FAgentHookPromptSubmittingHandler> PromptSubmitting;
    THookEntries<FAgentHookPromptSubmittedHandler> PromptSubmitted;
    THookEntries<FAgentHookToolBeforeHandler> ToolBefore;
    THookEntries<FAgentHookToolApprovalHandler> ToolApproval;
    THookEntries<FAgentHookToolReadyHandler> ToolReady;
    THookEntries<FAgentHookToolAfterHandler> ToolAfter;
    THookEntries<FAgentHookRunExitedHandler> RunExited;
    TPendingToolStates<FToolBeforeState> PendingBefore;
    TPendingToolStates<FToolApprovalState> PendingApproval;
};

FAgentHookRegistry::FAgentHookRegistry() : Impl(MakeUnique<FImpl>()) {}
FAgentHookRegistry::~FAgentHookRegistry() = default;

FAgentHookRegistry& FAgentHookRegistry::Get()
{
    static FAgentHookRegistry Registry;
    return Registry;
}

FAgentHookHandle FAgentHookRegistry::RegisterPromptSubmitting(FName Name, int32 Priority,
    FAgentHookPromptSubmittingHandler Handler)
{
    return AddEntry(Impl->PromptSubmitting, EAgentHookPoint::PromptSubmitting,
        Impl->NextId, Name, Priority, MoveTemp(Handler));
}

FAgentHookHandle FAgentHookRegistry::RegisterPromptSubmitted(FName Name, int32 Priority,
    FAgentHookPromptSubmittedHandler Handler)
{
    return AddEntry(Impl->PromptSubmitted, EAgentHookPoint::PromptSubmitted,
        Impl->NextId, Name, Priority, MoveTemp(Handler));
}

FAgentHookHandle FAgentHookRegistry::RegisterToolBefore(FName Name, int32 Priority,
    FAgentHookToolBeforeHandler Handler)
{
    return AddEntry(Impl->ToolBefore, EAgentHookPoint::ToolBefore,
        Impl->NextId, Name, Priority, MoveTemp(Handler));
}

FAgentHookHandle FAgentHookRegistry::RegisterToolApproval(FName Name, int32 Priority,
    FAgentHookToolApprovalHandler Handler)
{
    return AddEntry(Impl->ToolApproval, EAgentHookPoint::ToolApproval,
        Impl->NextId, Name, Priority, MoveTemp(Handler), true);
}

FAgentHookHandle FAgentHookRegistry::RegisterToolReady(FName Name, int32 Priority,
    FAgentHookToolReadyHandler Handler)
{
    return AddEntry(Impl->ToolReady, EAgentHookPoint::ToolReady,
        Impl->NextId, Name, Priority, MoveTemp(Handler));
}

FAgentHookHandle FAgentHookRegistry::RegisterToolAfter(FName Name, int32 Priority,
    FAgentHookToolAfterHandler Handler)
{
    return AddEntry(Impl->ToolAfter, EAgentHookPoint::ToolAfter,
        Impl->NextId, Name, Priority, MoveTemp(Handler));
}

FAgentHookHandle FAgentHookRegistry::RegisterRunExited(FName Name, int32 Priority,
    FAgentHookRunExitedHandler Handler)
{
    return AddEntry(Impl->RunExited, EAgentHookPoint::RunExited,
        Impl->NextId, Name, Priority, MoveTemp(Handler));
}

bool FAgentHookRegistry::Unregister(FAgentHookHandle Handle)
{
    check(IsInGameThread());
    if (!Handle.IsValid()) { return false; }
    switch (Handle.Point)
    {
    case EAgentHookPoint::PromptSubmitting: return RemoveEntry(Impl->PromptSubmitting, Handle.Id);
    case EAgentHookPoint::PromptSubmitted: return RemoveEntry(Impl->PromptSubmitted, Handle.Id);
    case EAgentHookPoint::ToolBefore: return RemoveEntry(Impl->ToolBefore, Handle.Id);
    case EAgentHookPoint::ToolApproval: return RemoveEntry(Impl->ToolApproval, Handle.Id);
    case EAgentHookPoint::ToolReady: return RemoveEntry(Impl->ToolReady, Handle.Id);
    case EAgentHookPoint::ToolAfter: return RemoveEntry(Impl->ToolAfter, Handle.Id);
    case EAgentHookPoint::RunExited: return RemoveEntry(Impl->RunExited, Handle.Id);
    }
    return false;
}

bool FAgentHookRegistry::IsRegistered(FAgentHookHandle Handle) const
{
    check(IsInGameThread());
    if (!Handle.IsValid()) { return false; }
    switch (Handle.Point)
    {
    case EAgentHookPoint::PromptSubmitting: return FindEntry(Impl->PromptSubmitting, Handle.Id).IsValid();
    case EAgentHookPoint::PromptSubmitted: return FindEntry(Impl->PromptSubmitted, Handle.Id).IsValid();
    case EAgentHookPoint::ToolBefore: return FindEntry(Impl->ToolBefore, Handle.Id).IsValid();
    case EAgentHookPoint::ToolApproval: return FindEntry(Impl->ToolApproval, Handle.Id).IsValid();
    case EAgentHookPoint::ToolReady: return FindEntry(Impl->ToolReady, Handle.Id).IsValid();
    case EAgentHookPoint::ToolAfter: return FindEntry(Impl->ToolAfter, Handle.Id).IsValid();
    case EAgentHookPoint::RunExited: return FindEntry(Impl->RunExited, Handle.Id).IsValid();
    }
    return false;
}

bool FAgentHookRegistry::EmitPromptSubmitting(FAgentHookPromptContext& Context, FString& OutReason)
{
    check(IsInGameThread());
    OutReason.Empty();
    const FGuid SessionId = Context.SessionId;
    const FGuid RunId = Context.RunId;
    const FString UserPrompt = Context.UserPrompt;
    const FString ModelId = Context.ModelId;
    const TArray<FString> CandidatePaths = Context.CandidatePaths;
    for (const uint64 Id : SnapshotIds(Impl->PromptSubmitting))
    {
        const auto Entry = FindEntry(Impl->PromptSubmitting, Id);
        if (!Entry) { continue; }
        const bool bAllow = Entry->Handler(Context, OutReason);
        if (Context.SessionId != SessionId || Context.RunId != RunId
            || Context.UserPrompt != UserPrompt || Context.ModelId != ModelId
            || Context.CandidatePaths != CandidatePaths)
        {
            OutReason = FString::Printf(TEXT("Prompt hook '%s' changed immutable input."),
                *Entry->Name.ToString());
            return false;
        }
        if (!bAllow)
        {
            if (OutReason.IsEmpty())
            {
                OutReason = FString::Printf(TEXT("Prompt hook '%s' rejected submission."),
                    *Entry->Name.ToString());
            }
            return false;
        }
        OutReason.Empty();
    }
    return true;
}

void FAgentHookRegistry::EmitPromptSubmitted(const FAgentHookPromptContext& Context)
{
    check(IsInGameThread());
    for (const uint64 Id : SnapshotIds(Impl->PromptSubmitted))
    {
        const auto Entry = FindEntry(Impl->PromptSubmitted, Id);
        if (Entry) { Entry->Handler(Context); }
    }
}

void FAgentHookRegistry::RunToolBefore(const FAgentHookToolContext& Context,
    FAgentHookToolBeforeCompletion Complete)
{
    check(IsInGameThread());
    if (!Complete) { return; }
    const TSharedRef<FToolBeforeState, ESPMode::ThreadSafe> State =
        MakeShared<FToolBeforeState, ESPMode::ThreadSafe>();
    State->Context = Context;
    State->HandlerIds = SnapshotIds(Impl->ToolBefore);
    State->Complete = MoveTemp(Complete);
    Impl->PendingBefore.FindOrAdd(Context.InvocationId).Add(State);
    const TWeakPtr<bool, ESPMode::ThreadSafe> WeakLifetime = Impl->Lifetime;
    const TWeakPtr<FToolBeforeState, ESPMode::ThreadSafe> WeakState = State;
    State->Advance = [this, WeakLifetime, WeakState]()
    {
        check(IsInGameThread());
        if (!WeakLifetime.IsValid()) { return; }
        const TSharedPtr<FToolBeforeState, ESPMode::ThreadSafe> Current = WeakState.Pin();
        if (!Current || Current->bFinished) { return; }
        while (Current->Index < Current->HandlerIds.Num())
        {
            const uint64 HandlerId = Current->HandlerIds[Current->Index];
            const auto Entry = FindEntry(Impl->ToolBefore, HandlerId);
            if (!Entry) { ++Current->Index; continue; }
            const TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> Continued =
                MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
            Entry->Handler(Current->Context,
                [this, WeakLifetime, State = Current.ToSharedRef(), HandlerId, Continued]
                    (FAgentHookToolBeforeResult Result) mutable
                {
                    if (Continued->exchange(true)) { return; }
                    auto Deliver = [this, WeakLifetime, State, HandlerId,
                        Result = MoveTemp(Result)]() mutable
                    {
                        if (!WeakLifetime.IsValid() || State->bFinished) { return; }
                        check(IsInGameThread());
                        // Unregistered handlers cannot decide a pending call.
                        if (!FindEntry(Impl->ToolBefore, HandlerId))
                        {
                            ++State->Index;
                            State->Advance();
                            return;
                        }
                        State->bNeedsVisibleEditorFrame |= Result.bNeedsVisibleEditorFrame;
                        if (Result.Decision == EAgentHookToolDecision::Continue)
                        {
                            ++State->Index;
                            State->Advance();
                            return;
                        }
                        State->bFinished = true;
                        Result.bNeedsVisibleEditorFrame = State->bNeedsVisibleEditorFrame;
                        RemovePendingState(Impl->PendingBefore,
                            State->Context.InvocationId, &State.Get());
                        FAgentHookToolBeforeCompletion Completion = MoveTemp(State->Complete);
                        Completion(MoveTemp(Result));
                    };
                    if (IsInGameThread()) { Deliver(); }
                    else { AsyncTask(ENamedThreads::GameThread, MoveTemp(Deliver)); }
                });
            return;
        }
        Current->bFinished = true;
        FAgentHookToolBeforeResult Result;
        Result.bNeedsVisibleEditorFrame = Current->bNeedsVisibleEditorFrame;
        RemovePendingState(Impl->PendingBefore,
            Current->Context.InvocationId, Current.Get());
        FAgentHookToolBeforeCompletion Completion = MoveTemp(Current->Complete);
        Completion(MoveTemp(Result));
    };
    State->Advance();
}

void FAgentHookRegistry::RequestToolApproval(const FAgentHookToolContext& Context,
    FAgentHookToolApprovalCompletion Complete)
{
    check(IsInGameThread());
    if (!Complete) { return; }
    if (Impl->ToolApproval.IsEmpty())
    {
        FAgentHookToolApprovalResult Result;
        Result.Reason = TEXT("Required tool approval handler is not registered.");
        Complete(MoveTemp(Result));
        return;
    }
    const auto Entry = Impl->ToolApproval[0];
    const uint64 HandlerId = Entry->Handle.Id;
    const TWeakPtr<bool, ESPMode::ThreadSafe> WeakLifetime = Impl->Lifetime;
    const TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> Continued =
        MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
    const TSharedRef<FToolApprovalState, ESPMode::ThreadSafe> State =
        MakeShared<FToolApprovalState, ESPMode::ThreadSafe>();
    State->InvocationId = Context.InvocationId;
    State->Complete = MoveTemp(Complete);
    Impl->PendingApproval.FindOrAdd(Context.InvocationId).Add(State);
    Entry->Handler(Context,
        [this, WeakLifetime, HandlerId, Continued, State]
            (FAgentHookToolApprovalResult Result) mutable
        {
            if (Continued->exchange(true)) { return; }
            auto Deliver = [this, WeakLifetime, HandlerId, State,
                Result = MoveTemp(Result)]() mutable
            {
                if (!WeakLifetime.IsValid() || State->bFinished) { return; }
                check(IsInGameThread());
                if (!FindEntry(Impl->ToolApproval, HandlerId))
                {
                    Result.bApproved = false;
                    Result.Reason = TEXT("Tool approval handler was unregistered before completing.");
                }
                State->bFinished = true;
                RemovePendingState(Impl->PendingApproval,
                    State->InvocationId, &State.Get());
                FAgentHookToolApprovalCompletion Callback = MoveTemp(State->Complete);
                Callback(MoveTemp(Result));
            };
            if (IsInGameThread()) { Deliver(); }
            else { AsyncTask(ENamedThreads::GameThread, MoveTemp(Deliver)); }
        });
}

void FAgentHookRegistry::CancelToolInvocation(FGuid InvocationId)
{
    check(IsInGameThread());
    if (TArray<TSharedPtr<FToolBeforeState, ESPMode::ThreadSafe>>* States =
        Impl->PendingBefore.Find(InvocationId))
    {
        for (const TSharedPtr<FToolBeforeState, ESPMode::ThreadSafe>& State : *States)
        {
            State->bFinished = true;
            State->Complete = FAgentHookToolBeforeCompletion();
        }
        Impl->PendingBefore.Remove(InvocationId);
    }
    if (TArray<TSharedPtr<FToolApprovalState, ESPMode::ThreadSafe>>* States =
        Impl->PendingApproval.Find(InvocationId))
    {
        for (const TSharedPtr<FToolApprovalState, ESPMode::ThreadSafe>& State : *States)
        {
            State->bFinished = true;
            State->Complete = FAgentHookToolApprovalCompletion();
        }
        Impl->PendingApproval.Remove(InvocationId);
    }
}

bool FAgentHookRegistry::EmitToolReady(const FAgentHookToolContext& Context)
{
    check(IsInGameThread());
    bool bNeedsVisibleEditorFrame = false;
    for (const uint64 Id : SnapshotIds(Impl->ToolReady))
    {
        const auto Entry = FindEntry(Impl->ToolReady, Id);
        if (Entry) { bNeedsVisibleEditorFrame |= Entry->Handler(Context); }
    }
    return bNeedsVisibleEditorFrame;
}

void FAgentHookRegistry::EmitToolAfter(const FAgentHookToolContext& Context,
    const FAgentHookToolOutcome& Outcome)
{
    check(IsInGameThread());
    for (const uint64 Id : SnapshotIds(Impl->ToolAfter))
    {
        const auto Entry = FindEntry(Impl->ToolAfter, Id);
        if (Entry) { Entry->Handler(Context, Outcome); }
    }
}

void FAgentHookRegistry::EmitRunExited(const FAgentHookRunExitContext& Context)
{
    check(IsInGameThread());
    for (const uint64 Id : SnapshotIds(Impl->RunExited))
    {
        const auto Entry = FindEntry(Impl->RunExited, Id);
        if (Entry) { Entry->Handler(Context); }
    }
}

void FAgentHookRegistry::Shutdown()
{
    check(IsInGameThread());
    Impl->Lifetime.Reset();
    for (auto& Pair : Impl->PendingBefore)
    {
        for (const TSharedPtr<FToolBeforeState, ESPMode::ThreadSafe>& State : Pair.Value)
        {
            State->bFinished = true;
            State->Complete = FAgentHookToolBeforeCompletion();
        }
    }
    Impl->PendingBefore.Empty();
    for (auto& Pair : Impl->PendingApproval)
    {
        for (const TSharedPtr<FToolApprovalState, ESPMode::ThreadSafe>& State : Pair.Value)
        {
            State->bFinished = true;
            State->Complete = FAgentHookToolApprovalCompletion();
        }
    }
    Impl->PendingApproval.Empty();
    Impl->PromptSubmitting.Empty();
    Impl->PromptSubmitted.Empty();
    Impl->ToolBefore.Empty();
    Impl->ToolApproval.Empty();
    Impl->ToolReady.Empty();
    Impl->ToolAfter.Empty();
    Impl->RunExited.Empty();
}
