#pragma once

#include "CoreMinimal.h"
#include "Serialization/JsonSerializer.h"

/**
 * JSON helpers shared by the Agent Workbench translation units.
 *
 * The module builds as a single unity translation unit, so two .cpp files that both define
 * the same helper in an anonymous namespace collide (MSVC C2084). Defining the helper once
 * here and pulling it in with a using-declaration keeps a single definition per translation
 * unit while regular (non-unity) builds still link through the inline definitions.
 */
namespace AgentWorkbenchJson
{
    /** Serializes an object into a compact JSON string. */
    inline FString SerializeJson(const TSharedRef<FJsonObject>& Object)
    {
        FString Text;
        FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Text));
        return Text;
    }

    /** Parses Text into OutObject; OutObject is reset when the text is not a JSON object. */
    inline bool ParseObject(const FString& Text, TSharedPtr<FJsonObject>& OutObject)
    {
        OutObject.Reset();
        return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), OutObject) && OutObject.IsValid();
    }
}
