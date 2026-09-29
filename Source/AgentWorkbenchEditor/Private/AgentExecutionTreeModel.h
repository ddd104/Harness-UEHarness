#pragma once

#include "CoreMinimal.h"

struct FAgentEvent;
class FAgentSession;

enum class EAgentExecutionNodeKind : uint8
{
    Run,
    Step,
    Detail
};

enum class EAgentExecutionRunStatus : uint8
{
    Unknown,
    Running,
    Completed,
    Failed,
    Cancelled,
    Interrupted
};

struct FAgentExecutionNode
{
    EAgentExecutionNodeKind Kind = EAgentExecutionNodeKind::Run;
    FGuid RunId;
    int32 Sequence = 0;
    FString Title;
    FString DisplayDetail;
    TSharedPtr<FAgentEvent> Event;
    TArray<TSharedPtr<FAgentExecutionNode>> Children;
};

class FAgentExecutionTreeModel
{
public:
    static TArray<TSharedPtr<FAgentExecutionNode>> Build(const FAgentSession& Session);
    static EAgentExecutionRunStatus StatusFor(const FAgentSession& Session, const FAgentExecutionNode& Run);
};
