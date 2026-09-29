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

/** Runs optional pre-execution steps, then delegates the actual call to the bridge. */
class FAgentToolWorkflow
{
public:
    void AddGate(const TSharedRef<IAgentToolGate>& Gate);
    void Execute(const FAgentToolInvocation& Invocation, double TimeoutSeconds,
        TFunction<void(FMCPToolResult)> Complete) const;

private:
    TArray<TSharedRef<IAgentToolGate>> Gates;
};
