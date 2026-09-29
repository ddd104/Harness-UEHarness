#pragma once

#include "CoreMinimal.h"

struct FAgentEvent;
struct FAgentMessage;

// Presentation only: never feed this output back into the model or persist it as history.
class FAgentWorkbenchDisplayFormatter
{
public:
    static FString FormatEventDetail(const FAgentEvent& Event);
    static TArray<TSharedPtr<FAgentMessage>> VisibleConversationMessages(
        const TArray<TSharedPtr<FAgentMessage>>& Messages);
};
