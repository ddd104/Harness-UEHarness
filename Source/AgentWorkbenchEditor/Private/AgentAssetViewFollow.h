#pragma once

#include "AgentToolWorkflow.h"

/** Best-effort editor navigation around approved Blueprint and material edits. */
class FAgentAssetViewFollow final : public IAgentToolExecutionObserver
{
public:
    virtual TSharedPtr<IAgentToolObserverState> BeforeExecute(
        const FAgentResolvedToolInvocation& Tool) override;
    virtual void AfterExecute(const FAgentResolvedToolInvocation& Tool,
        const TSharedPtr<IAgentToolObserverState>& State, const FMCPToolResult& Result) override;
};
