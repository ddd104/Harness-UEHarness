#include "AgentWorkbenchSettings.h"

UAgentWorkbenchSettings::UAgentWorkbenchSettings()
    : ApiKeyEnvironmentVariable(TEXT("UE_AGENT_API_KEY"))
    , BaseUrlEnvironmentVariable(TEXT("UE_AGENT_BASE_URL"))
    , ProviderType(EAgentWorkbenchProvider::DeepSeek)
    , DefaultModel(TEXT("deepseek-flash"))
    , RequestTimeoutSeconds(120)
    , ToolTimeoutSeconds(60)
    , RunTimeoutSeconds(900)
    , MaxToolSteps(16)
    , MaxConcurrentRuns(4)
    , MaxCandidateAssets(64)
    , bPersistDetailedPayloads(false)
{
    AvailableModels.Add(TEXT("deepseek-flash"));
}
