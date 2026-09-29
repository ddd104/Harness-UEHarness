#include "AgentToolSchemaValidator.h"

#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
bool ParseJsonObject(const TCHAR* Text, TSharedPtr<FJsonObject>& OutObject)
{
    const FString Source(Text);
    return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Source), OutObject)
        && OutObject.IsValid();
}

bool ValidateText(const TCHAR* SchemaText, const TCHAR* ArgumentsText, FString& OutError)
{
    TSharedPtr<FJsonObject> Schema;
    TSharedPtr<FJsonObject> Arguments;
    if (!ParseJsonObject(SchemaText, Schema) || !ParseJsonObject(ArgumentsText, Arguments))
    {
        OutError = TEXT("test fixture contains invalid JSON");
        return false;
    }
    return FAgentToolSchemaValidator::Validate(Schema, Arguments.ToSharedRef(), OutError);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentToolSchemaNestedArgumentsTest,
    "AgentWorkbench.Tools.Schema.NestedArguments",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentToolSchemaNestedArgumentsTest::RunTest(const FString& Parameters)
{
    const TCHAR* Schema = TEXT(R"({
        "type":"object","required":["request"],"properties":{
            "request":{"type":"object","required":["name"],"properties":{
                "name":{"type":"string","minLength":2,"maxLength":4,"pattern":"^[A-Z]+$"},
                "index":{"type":"integer","minimum":1,"maximum":3}
            }}
        }
    })");
    FString Error;
    TestTrue(TEXT("valid nested arguments pass"), ValidateText(Schema,
        TEXT(R"({"request":{"name":"AB","index":2}})"), Error));
    TestTrue(TEXT("missing required property fails"), !ValidateText(Schema,
        TEXT(R"({"request":{"index":2}})"), Error) && Error.Contains(TEXT("name")));
    TestTrue(TEXT("fractional integer fails"), !ValidateText(Schema,
        TEXT(R"({"request":{"name":"AB","index":1.5}})"), Error) && Error.Contains(TEXT("index")));
    TestTrue(TEXT("pattern mismatch fails"), !ValidateText(Schema,
        TEXT(R"({"request":{"name":"Ab"}})"), Error) && Error.Contains(TEXT("pattern")));
    TestTrue(TEXT("unknown nested property is rejected by default"), !ValidateText(Schema,
        TEXT(R"({"request":{"name":"AB","ignored":1}})"), Error) && Error.Contains(TEXT("ignored")));
    TestTrue(TEXT("unknown root property is rejected by default"), !ValidateText(Schema,
        TEXT(R"({"request":{"name":"AB"},"ignored":1})"), Error) && Error.Contains(TEXT("ignored")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentToolSchemaCollectionsTest,
    "AgentWorkbench.Tools.Schema.Collections",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentToolSchemaCollectionsTest::RunTest(const FString& Parameters)
{
    const TCHAR* Schema = TEXT(R"({"type":"object","properties":{
        "values":{"type":"array","minItems":2,"maxItems":3,"uniqueItems":true,
            "items":{"type":"number","multipleOf":0.5}},
        "flags":{"type":"object","minProperties":1,"maxProperties":2,
            "additionalProperties":{"type":"boolean"}}
    }})");
    FString Error;
    TestTrue(TEXT("bounded array and typed free dictionary pass"), ValidateText(Schema,
        TEXT(R"({"values":[1,1.5],"flags":{"a":true}})"), Error));
    TestTrue(TEXT("duplicate array item fails"), !ValidateText(Schema,
        TEXT(R"({"values":[1,1],"flags":{"a":true}})"), Error) && Error.Contains(TEXT("uniqueItems")));
    TestTrue(TEXT("array size bound fails"), !ValidateText(Schema,
        TEXT(R"({"values":[1],"flags":{"a":true}})"), Error) && Error.Contains(TEXT("minItems")));
    TestTrue(TEXT("multipleOf bound fails"), !ValidateText(Schema,
        TEXT(R"({"values":[1,1.2],"flags":{"a":true}})"), Error) && Error.Contains(TEXT("multipleOf")));
    TestTrue(TEXT("tiny nonzero value cannot round to zero multiple"), !ValidateText(Schema,
        TEXT(R"({"values":[1,0.00000000000000000001],"flags":{"a":true}})"), Error)
        && Error.Contains(TEXT("multipleOf")));
    TestTrue(TEXT("object size bound fails"), !ValidateText(Schema,
        TEXT(R"({"values":[1,1.5],"flags":{}})"), Error) && Error.Contains(TEXT("minProperties")));
    TestTrue(TEXT("additional property schema is enforced"), !ValidateText(Schema,
        TEXT(R"({"values":[1,1.5],"flags":{"a":1}})"), Error) && Error.Contains(TEXT("a")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentToolSchemaClosedObjectTest,
    "AgentWorkbench.Tools.Schema.ClosedObject",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentToolSchemaClosedObjectTest::RunTest(const FString& Parameters)
{
    FString Error;
    TestTrue(TEXT("zero-argument schema accepts empty object"), ValidateText(
        TEXT(R"({"type":"object"})"), TEXT(R"({})"), Error));
    TestTrue(TEXT("zero-argument schema rejects ignored parameters"), !ValidateText(
        TEXT(R"({"type":"object"})"), TEXT(R"({"ignored":1})"), Error) && Error.Contains(TEXT("ignored")));
    TestTrue(TEXT("explicit additionalProperties true accepts dictionary"), ValidateText(
        TEXT(R"({"type":"object","additionalProperties":true})"), TEXT(R"({"entry":1})"), Error));
    TestTrue(TEXT("enum compares objects without depending on key order"), ValidateText(
        TEXT(R"({"type":"object","properties":{"value":{"enum":[{"a":1,"b":2}]}}})"),
        TEXT(R"({"value":{"b":2,"a":1}})"), Error));
    TestTrue(TEXT("const object permits only its exact contents"), ValidateText(
        TEXT(R"({"type":"object","properties":{"value":{"const":{"a":1}}}})"),
        TEXT(R"({"value":{"a":1}})"), Error));
    TestTrue(TEXT("enum still rejects a different object"), !ValidateText(
        TEXT(R"({"type":"object","properties":{"value":{"enum":[{"a":1}]}}})"),
        TEXT(R"({"value":{"a":2}})"), Error) && Error.Contains(TEXT("enum")));
    TestTrue(TEXT("explicit properties remain closed after enum match"), !ValidateText(
        TEXT(R"({"type":"object","properties":{"value":{"enum":[{"a":1}],"properties":{}}}})"),
        TEXT(R"({"value":{"a":1}})"), Error) && Error.Contains(TEXT("a")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentToolSchemaRejectsUnsupportedTest,
    "AgentWorkbench.Tools.Schema.RejectsUnsupported",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentToolSchemaRejectsUnsupportedTest::RunTest(const FString& Parameters)
{
    FString Error;
    TestTrue(TEXT("unsupported constraint in unused optional property fails closed"), !ValidateText(
        TEXT(R"({"type":"object","properties":{"unused":{"not":{"type":"string"}}}})"),
        TEXT(R"({})"), Error) && Error.Contains(TEXT("not")));
    TestTrue(TEXT("malformed uniqueItems fails closed"), !ValidateText(
        TEXT(R"({"type":"object","properties":{"list":{"type":"array","uniqueItems":"yes"}}})"),
        TEXT(R"({})"), Error) && Error.Contains(TEXT("uniqueItems")));
    TestTrue(TEXT("malformed size bound fails closed"), !ValidateText(
        TEXT(R"({"type":"object","maxProperties":-1})"), TEXT(R"({})"), Error)
        && Error.Contains(TEXT("maxProperties")));
    TestTrue(TEXT("invalid pattern in unused optional property fails closed"), !ValidateText(
        TEXT(R"({"type":"object","properties":{"unused":{"type":"string","pattern":"["}}})"),
        TEXT(R"({})"), Error) && Error.Contains(TEXT("pattern")));
    const TSharedRef<FJsonObject> DeepSchema = MakeShared<FJsonObject>();
    DeepSchema->SetStringField(TEXT("type"), TEXT("object"));
    TSharedPtr<FJsonValue> DeepValue = MakeShared<FJsonValueNumber>(0);
    for (int32 Index = 0; Index < 40; ++Index)
    {
        TArray<TSharedPtr<FJsonValue>> Array;
        Array.Add(DeepValue);
        DeepValue = MakeShared<FJsonValueArray>(MoveTemp(Array));
    }
    DeepSchema->SetField(TEXT("default"), DeepValue);
    TestTrue(TEXT("schema depth limit fails closed"),
        !FAgentToolSchemaValidator::Validate(DeepSchema, MakeShared<FJsonObject>(), Error)
        && Error.Contains(TEXT("depth")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentToolSchemaCompositionsTest,
    "AgentWorkbench.Tools.Schema.Compositions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentToolSchemaCompositionsTest::RunTest(const FString& Parameters)
{
    const TCHAR* Schema = TEXT(R"({"type":"object","properties":{"value":{"oneOf":[
        {"type":"object","required":["text"],"properties":{"text":{"type":"string"}}},
        {"type":"object","required":["number"],"properties":{"number":{"type":"number"}}}
    ]}}})");
    FString Error;
    TestTrue(TEXT("oneOf selects a matching object branch"), ValidateText(Schema,
        TEXT(R"({"value":{"text":"hello"}})"), Error));
    TestTrue(TEXT("oneOf rejects a value matching no branch"), !ValidateText(Schema,
        TEXT(R"({"value":{"unknown":true}})"), Error) && Error.Contains(TEXT("oneOf")));
    TestTrue(TEXT("oneOf rejects multiple matching branches"), !ValidateText(
        TEXT(R"({"type":"object","properties":{"value":{"oneOf":[{"type":"number"},{"minimum":0}]}}})"),
        TEXT(R"({"value":1})"), Error) && Error.Contains(TEXT("oneOf")));
    TestTrue(TEXT("anyOf accepts one matching branch"), ValidateText(
        TEXT(R"({"type":"object","properties":{"value":{"anyOf":[{"type":"number"},{"type":"string"}]}}})"),
        TEXT(R"({"value":"hello"})"), Error));
    TestTrue(TEXT("allOf enforces every branch"), !ValidateText(
        TEXT(R"({"type":"object","properties":{"value":{"allOf":[{"type":"number"},{"minimum":2}]}}})"),
        TEXT(R"({"value":1})"), Error) && Error.Contains(TEXT("allOf")));
    TestTrue(TEXT("empty composition is rejected in an unused optional property"), !ValidateText(
        TEXT(R"({"type":"object","properties":{"unused":{"oneOf":[]}}})"),
        TEXT(R"({})"), Error) && Error.Contains(TEXT("oneOf")));
    return true;
}

#endif
