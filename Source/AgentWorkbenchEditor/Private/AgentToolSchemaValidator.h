#pragma once

#include "Dom/JsonObject.h"

/** Validates the supported JSON Schema subset before an external UE MCP tool is invoked. */
struct FAgentToolSchemaValidator
{
    static bool Validate(const TSharedPtr<FJsonObject>& Schema,
        const TSharedRef<FJsonObject>& Arguments, FString& OutError);
};
