#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "AgentWorkbenchSettings.generated.h"

UENUM()
enum class EAgentWorkbenchProvider : uint8
{
    DeepSeek UMETA(DisplayName="DeepSeek"),
    OpenAI UMETA(DisplayName="OpenAI"),
    Anthropic UMETA(DisplayName="Anthropic"),
    Gemini UMETA(DisplayName="Google Gemini"),
    OpenRouter UMETA(DisplayName="OpenRouter"),
    Ollama UMETA(DisplayName="Ollama (local)")
};

UCLASS(Config=Game, DefaultConfig, meta=(DisplayName="Agent Workbench"))
class AGENTWORKBENCHEDITOR_API UAgentWorkbenchSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    UAgentWorkbenchSettings();
    virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

    // Names only. Secret values are never stored in project settings.
    UPROPERTY(Config, EditAnywhere, Category="Connection") FString ApiKeyEnvironmentVariable;
    UPROPERTY(Config, EditAnywhere, Category="Connection") FString BaseUrlEnvironmentVariable;
    UPROPERTY(Config, EditAnywhere, Category="Connection") EAgentWorkbenchProvider ProviderType;
    UPROPERTY(Config, EditAnywhere, Category="Connection") FString DefaultModel;
    UPROPERTY(Config, EditAnywhere, Category="Connection") TArray<FString> AvailableModels;
    UPROPERTY(Config, EditAnywhere, Category="Limits", meta=(ClampMin="1")) int32 RequestTimeoutSeconds;
    UPROPERTY(Config, EditAnywhere, Category="Limits", meta=(ClampMin="1")) int32 ToolTimeoutSeconds;
    UPROPERTY(Config, EditAnywhere, Category="Limits", meta=(ClampMin="1")) int32 RunTimeoutSeconds;
    UPROPERTY(Config, EditAnywhere, Category="Limits", meta=(ClampMin="1")) int32 MaxToolSteps;
    UPROPERTY(Config, EditAnywhere, Category="Limits", meta=(ClampMin="1")) int32 MaxConcurrentRuns;
    UPROPERTY(Config, EditAnywhere, Category="Limits", meta=(ClampMin="1")) int32 MaxCandidateAssets;
    UPROPERTY(Config, EditAnywhere, Category="Privacy") bool bPersistDetailedPayloads;
};
