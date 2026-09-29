#pragma once

#include "AgentWorkbenchSession.h"
#include "MCPToolTypes.h"

/** A model proposal, kept separate from both provider parsing and tool execution. */
struct FAgentToolInvocation
{
    TSharedRef<const FAgentRunInputSnapshot> Snapshot;
    TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> CancellationToken;
    FGuid RequestId;
    FAgentToolCall Call;
    // The Run deadline is independent of the tool's execution timeout.
    double RunDeadlineSeconds = 0.0;
};

using FAgentToolGateContinuation = TFunction<void(bool /* bAllow */, FString /* Reason */)>;

struct FAgentResolvedToolInvocation
{
    const FAgentToolInvocation& Invocation;
    FString RegistryName;
    FString DisplayName;
    TSharedRef<FJsonObject> Arguments;
};

class IAgentToolObserverState
{
public:
    virtual ~IAgentToolObserverState() = default;
    virtual bool NeedsVisibleEditorFrame() const { return false; }
};

/** Optional editor/UI work around a tool call. It cannot replace its result. */
class IAgentToolExecutionObserver
{
public:
    virtual ~IAgentToolExecutionObserver() = default;
    virtual TSharedPtr<IAgentToolObserverState> BeforeExecute(
        const FAgentResolvedToolInvocation& Tool) = 0;
    virtual void BeforeDispatch(const FAgentResolvedToolInvocation&,
        const TSharedPtr<IAgentToolObserverState>&) {}
    virtual void AfterExecute(const FAgentResolvedToolInvocation& Tool,
        const TSharedPtr<IAgentToolObserverState>& State, const FMCPToolResult& Result) = 0;
};

/** Future approval, scheduling or context steps can finish asynchronously.
 * Continue may be called from any thread; duplicate completions are ignored.
 */
class IAgentToolGate
{
public:
    virtual ~IAgentToolGate() = default;
    virtual void Evaluate(const FAgentToolInvocation& Invocation,
        FAgentToolGateContinuation Continue) = 0;
};

struct FAgentCompilationTracker;

/** Runs optional pre-execution steps, then delegates the actual call to the bridge. */
class FAgentToolWorkflow
{
public:
    FAgentToolWorkflow();
    void AddGate(const TSharedRef<IAgentToolGate>& Gate);
    void AddObserver(const TSharedRef<IAgentToolExecutionObserver>& Observer);
    void Execute(const FAgentToolInvocation& Invocation, double TimeoutSeconds,
        TFunction<void(FMCPToolResult)> Complete) const;

private:
    TArray<TSharedRef<IAgentToolGate>> Gates;
    TArray<TSharedRef<IAgentToolExecutionObserver>> Observers;
    TSharedPtr<FAgentCompilationTracker> CompilationTracker;
};
