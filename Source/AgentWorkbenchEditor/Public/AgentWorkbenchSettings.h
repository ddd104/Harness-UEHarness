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

/**
 * A skill source directory. The details panel shows a folder picker that stores the
 * path relative to the project root, so project settings stay portable; a folder
 * outside the project keeps its absolute path because it cannot be made relative.
 */
USTRUCT()
struct AGENTWORKBENCHEDITOR_API FAgentSkillDirectory
{
    GENERATED_BODY()

    /** Directory path relative to the project root (absolute when outside it). */
    UPROPERTY(EditAnywhere, Category="Path", meta=(DisplayName="Directory"))
    FString Path;
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
    UPROPERTY(Config, EditAnywhere, Category="Subagent") bool bEnableSubagentTool;
    UPROPERTY(Config, EditAnywhere, Category="Subagent", meta=(ClampMin="1")) int32 SubagentMaxTurns;
    UPROPERTY(Config, EditAnywhere, Category="Subagent", meta=(ClampMin="1")) int32 SubagentTimeoutSeconds;
    UPROPERTY(Config, EditAnywhere, Category="Subagent", meta=(ClampMin="1")) int32 SubagentMaxOutputTokens;
    // Context Compact (s08): character-count thresholds for the pre-request pipeline.
    UPROPERTY(Config, EditAnywhere, Category="Context Compact") bool bEnableContextCompact;
    UPROPERTY(Config, EditAnywhere, Category="Context Compact", meta=(ClampMin="1000")) int32 ContextCharLimit;
    UPROPERTY(Config, EditAnywhere, Category="Context Compact", meta=(ClampMin="1000")) int32 ToolResultBudgetChars;
    UPROPERTY(Config, EditAnywhere, Category="Context Compact", meta=(ClampMin="1000")) int32 LargeResultCharLimit;
    UPROPERTY(Config, EditAnywhere, Category="Context Compact", meta=(ClampMin="4")) int32 SnipMaxMessages;
    UPROPERTY(Config, EditAnywhere, Category="Context Compact", meta=(ClampMin="0")) int32 KeepRecentResults;
    UPROPERTY(Config, EditAnywhere, Category="Context Compact", meta=(ClampMin="0")) int32 KeepRecentMessages;
    UPROPERTY(Config, EditAnywhere, Category="Context Compact", meta=(ClampMin="0", ClampMax="3")) int32 MaxReactiveRetries;
    UPROPERTY(Config, EditAnywhere, Category="Context Compact", meta=(ClampMin="512")) int32 SummaryMaxOutputTokens;
    // Filesystem directories of file-based skills. Each entry is a directory whose
    // immediate subdirectories contain a SKILL.md; use the folder picker to add one.
    // Entries are stored relative to the project root (absolute when outside it) and
    // are scanned in order, so an earlier directory wins a duplicate skill name.
    // An empty array means "<Project>/Skills/".
    UPROPERTY(Config, EditAnywhere, Category="Skills",
        meta=(DisplayName="Skill Directories", TitleProperty="Path"))
    TArray<FAgentSkillDirectory> SkillDirectories;
    UPROPERTY(Config, EditAnywhere, Category="Privacy") bool bPersistDetailedPayloads;
};
