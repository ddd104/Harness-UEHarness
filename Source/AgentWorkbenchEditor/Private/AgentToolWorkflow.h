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

struct FAgentResolvedToolInvocation
{
    const FAgentToolInvocation& Invocation;
    FString RegistryName;
    FString DisplayName;
    TSharedRef<const FJsonObject> Arguments;
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

/** Resolves and executes a call; lifecycle extensions live in BSHarnessHooks. */
class FAgentToolWorkflow
{
public:
    void Execute(const FAgentToolInvocation& Invocation, double TimeoutSeconds,
        TFunction<void(FMCPToolResult)> Complete) const;
};

/** Called by the editor module after dependencies start, and before they shut down. */
void RegisterAgentWorkbenchToolHooks();
void UnregisterAgentWorkbenchToolHooks();
