#include "AgentWorkbenchDisplayFormatter.h"

#include "AgentWorkbenchSession.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif

namespace
{
constexpr int32 MaxDisplayChars = 32768;
constexpr int32 MaxParseChars = 524288;
constexpr int32 MaxJsonDepth = 12;
constexpr TCHAR TruncationNotice[] = TEXT("\n…（内容过长，已截断）");

class FDisplayWriter
{
public:
    bool IsTruncated() const { return bTruncated; }

    void Append(const FString& Text)
    {
        if (bTruncated) { return; }
        const int32 Remaining = MaxDisplayChars - Output.Len();
        if (Text.Len() <= Remaining)
        {
            Output += Text;
            return;
        }
        if (Remaining > 0) { Output += Text.Left(Remaining); }
        bTruncated = true;
    }

    void AppendLine(const FString& Text = FString())
    {
        Append(Text);
        Append(TEXT("\n"));
    }

    void AppendIndentedText(const FString& Text, int32 Depth)
    {
        // JSON decoding has already converted escapes to characters. Keep literal
        // backslashes, including those used in source and Windows paths, intact.
        const FString ContinuationIndent = FString::ChrN(FMath::Max(0, Depth) * 2, TEXT(' '));
        FString WithIndent = Text.Left(MaxDisplayChars);
        WithIndent.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
        WithIndent.ReplaceInline(TEXT("\r"), TEXT("\n"));
        WithIndent.ReplaceInline(TEXT("\n"), *(FString(TEXT("\n")) + ContinuationIndent));
        Append(WithIndent);
    }

    FString Finish()
    {
        if (Output.EndsWith(TEXT("\n"))) { Output.LeftChopInline(1); }
        if (bTruncated) { Output += TruncationNotice; }
        return MoveTemp(Output);
    }

private:
    FString Output;
    bool bTruncated = false;
};

bool ParseJson(const FString& Text, TSharedPtr<FJsonValue>& OutValue)
{
    if (Text.Len() > MaxParseChars) { return false; }
    const FString Trimmed = Text.TrimStartAndEnd();
    if (Trimmed.IsEmpty()) { return false; }
    const TCHAR First = Trimmed[0];
    if (First != TEXT('{') && First != TEXT('[') && First != TEXT('"')) { return false; }
    return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Trimmed), OutValue) && OutValue.IsValid();
}

bool IsContainer(const TSharedPtr<FJsonValue>& Value)
{
    return Value.IsValid() && (Value->Type == EJson::Object || Value->Type == EJson::Array);
}

TSharedPtr<FJsonValue> NestedJsonString(const TSharedPtr<FJsonValue>& Value)
{
    if (!Value.IsValid() || Value->Type != EJson::String) { return nullptr; }
    TSharedPtr<FJsonValue> Nested;
    return ParseJson(Value->AsString(), Nested) && IsContainer(Nested) ? Nested : nullptr;
}

void WriteJsonValue(FDisplayWriter& Writer, const TSharedPtr<FJsonValue>& Value, int32 Depth);

void WriteChild(FDisplayWriter& Writer, const FString& Prefix,
    const TSharedPtr<FJsonValue>& Value, int32 Depth)
{
    if (Writer.IsTruncated()) { return; }
    Writer.Append(Prefix);
    if (!Value.IsValid()) { Writer.AppendLine(TEXT("null")); return; }
    TSharedPtr<FJsonValue> DisplayValue = NestedJsonString(Value);
    if (!DisplayValue) { DisplayValue = Value; }
    if (IsContainer(DisplayValue))
    {
        Writer.AppendLine();
        WriteJsonValue(Writer, DisplayValue, Depth + 1);
    }
    else
    {
        WriteJsonValue(Writer, DisplayValue, Depth + 1);
        Writer.AppendLine();
    }
}

void WriteJsonValue(FDisplayWriter& Writer, const TSharedPtr<FJsonValue>& Value, int32 Depth)
{
    if (Writer.IsTruncated()) { return; }
    if (!Value.IsValid()) { Writer.Append(TEXT("null")); return; }
    if (Depth >= MaxJsonDepth)
    {
        Writer.Append(TEXT("…（嵌套层级过深）"));
        return;
    }
    const FString Indent = FString::ChrN(Depth * 2, TEXT(' '));
    switch (Value->Type)
    {
    case EJson::Object:
    {
        const TSharedPtr<FJsonObject> Object = Value->AsObject();
        if (!Object.IsValid() || Object->Values.IsEmpty())
        {
            Writer.AppendLine(Indent + TEXT("{}"));
            return;
        }
        for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
        {
            if (Writer.IsTruncated()) { break; }
            WriteChild(Writer, Indent + Field.Key + TEXT(": "), Field.Value, Depth);
        }
        break;
    }
    case EJson::Array:
    {
        const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
        if (Items.IsEmpty())
        {
            Writer.AppendLine(Indent + TEXT("[]"));
            return;
        }
        for (const TSharedPtr<FJsonValue>& Item : Items)
        {
            if (Writer.IsTruncated()) { break; }
            WriteChild(Writer, Indent + TEXT("- "), Item, Depth);
        }
        break;
    }
    case EJson::String:
        Writer.AppendIndentedText(Value->AsString(), Depth);
        break;
    case EJson::Number:
        Writer.Append(Value->AsNumber() == FMath::FloorToDouble(Value->AsNumber())
            ? FString::Printf(TEXT("%.0f"), Value->AsNumber())
            : FString::SanitizeFloat(Value->AsNumber()));
        break;
    case EJson::Boolean:
        Writer.Append(Value->AsBool() ? TEXT("true") : TEXT("false"));
        break;
    case EJson::Null:
    default:
        Writer.Append(TEXT("null"));
        break;
    }
}
}

FString FAgentWorkbenchDisplayFormatter::FormatEventDetail(const FAgentEvent& Event)
{
    if (Event.Detail.IsEmpty()) { return FString(); }
    FDisplayWriter Writer;
    switch (Event.Type)
    {
    case EAgentEventType::ModelRequestStarted: Writer.AppendLine(TEXT("模型调用输入")); break;
    case EAgentEventType::ToolCallStarted: Writer.AppendLine(TEXT("工具调用参数")); break;
    case EAgentEventType::ToolCallCompleted: Writer.AppendLine(TEXT("工具调用结果")); break;
    default: break;
    }
    TSharedPtr<FJsonValue> Parsed;
    if (ParseJson(Event.Detail, Parsed))
    {
        const TSharedPtr<FJsonValue> Nested = NestedJsonString(Parsed);
        WriteJsonValue(Writer, Nested.IsValid() ? Nested : Parsed, 0);
    }
    else
    {
        Writer.Append(Event.Detail);
    }
    return Writer.Finish();
}

TArray<TSharedPtr<FAgentMessage>> FAgentWorkbenchDisplayFormatter::VisibleConversationMessages(
    const TArray<TSharedPtr<FAgentMessage>>& Messages)
{
    TMap<FGuid, int32> LastFinalAssistantIndex;
    for (int32 Index = 0; Index < Messages.Num(); ++Index)
    {
        const TSharedPtr<FAgentMessage>& Message = Messages[Index];
        if (Message && Message->Role == EAgentMessageRole::Assistant && Message->RunId.IsValid()
            && Message->ToolCalls.IsEmpty() && !Message->Text.TrimStartAndEnd().IsEmpty())
        {
            LastFinalAssistantIndex.Add(Message->RunId, Index);
        }
    }

    TArray<TSharedPtr<FAgentMessage>> Visible;
    for (int32 Index = 0; Index < Messages.Num(); ++Index)
    {
        const TSharedPtr<FAgentMessage>& Message = Messages[Index];
        if (!Message) { continue; }
        const int32* LastIndex = LastFinalAssistantIndex.Find(Message->RunId);
        const bool bLegacyAssistant = Message->Role == EAgentMessageRole::Assistant
            && !Message->RunId.IsValid() && Message->ToolCalls.IsEmpty()
            && !Message->Text.TrimStartAndEnd().IsEmpty();
        if (Message->Role == EAgentMessageRole::User || bLegacyAssistant
            || (Message->Role == EAgentMessageRole::Assistant && LastIndex && *LastIndex == Index))
        {
            Visible.Add(Message);
        }
    }
    return Visible;
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentWorkbenchDisplayFormatterTest,
    "AgentWorkbench.Display.Formatter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentWorkbenchDisplayFormatterTest::RunTest(const FString& Parameters)
{
    FAgentEvent Request;
    Request.Type = EAgentEventType::ModelRequestStarted;
    Request.Detail = TEXT(R"({"content":"first\nsecond\tcolumn","url":"https:\/\/example.com\/a"})");
    const FString RequestDisplay = FAgentWorkbenchDisplayFormatter::FormatEventDetail(Request);
    TestTrue(TEXT("Request is labeled"), RequestDisplay.Contains(TEXT("模型调用输入")));
    TestTrue(TEXT("JSON newline decoded"), RequestDisplay.Contains(TEXT("first\n  second")));
    TestTrue(TEXT("JSON tab decoded"), RequestDisplay.Contains(TEXT("second\tcolumn")));
    TestTrue(TEXT("JSON slash decoded"), RequestDisplay.Contains(TEXT("https://example.com/a")));
    TestFalse(TEXT("No newline escape remains"), RequestDisplay.Contains(TEXT("\\n")));
    TestFalse(TEXT("No tab escape remains"), RequestDisplay.Contains(TEXT("\\t")));
    TestFalse(TEXT("No slash escape remains"), RequestDisplay.Contains(TEXT("\\/")));

    FAgentEvent ToolResult;
    ToolResult.Type = EAgentEventType::ToolCallCompleted;
    ToolResult.Detail = TEXT(R"({"result":"{\"summary\":\"line 1\\nline 2\\tend\",\"path\":\"C:\\\\Temp\\\\file.txt\"}"})");
    const FString ToolDisplay = FAgentWorkbenchDisplayFormatter::FormatEventDetail(ToolResult);
    TestTrue(TEXT("Nested JSON newline decoded"), ToolDisplay.Contains(TEXT("line 1\n    line 2")));
    TestTrue(TEXT("Nested JSON tab decoded"), ToolDisplay.Contains(TEXT("line 2\tend")));
    TestTrue(TEXT("Windows path preserved"), ToolDisplay.Contains(TEXT("C:\\Temp\\file.txt")));
    TestFalse(TEXT("Nested newline escape decoded"), ToolDisplay.Contains(TEXT("\\n")));

    FAgentEvent EmptyContainers;
    EmptyContainers.Type = EAgentEventType::ToolCallStarted;
    EmptyContainers.Detail = TEXT(R"({"first":{},"second":[],"third":1})");
    const FString EmptyContainersDisplay = FAgentWorkbenchDisplayFormatter::FormatEventDetail(EmptyContainers);
    TestTrue(TEXT("Empty object ends its own line"), EmptyContainersDisplay.Contains(TEXT("{}\n")));
    TestTrue(TEXT("Empty array ends its own line"), EmptyContainersDisplay.Contains(TEXT("[]\n")));

    FAgentEvent PlainText;
    PlainText.Type = EAgentEventType::RunFailed;
    PlainText.Detail = TEXT("C:\\Temp\\file.txt has literal \\n text");
    const FString PlainDisplay = FAgentWorkbenchDisplayFormatter::FormatEventDetail(PlainText);
    TestEqual(TEXT("Plain text backslashes preserved"), PlainDisplay, PlainText.Detail);

    FAgentEvent Empty;
    Empty.Type = EAgentEventType::ToolCallStarted;
    TestTrue(TEXT("Empty detail stays empty"), FAgentWorkbenchDisplayFormatter::FormatEventDetail(Empty).IsEmpty());

    FAgentEvent Large;
    Large.Detail = FString::ChrN(MaxDisplayChars + 100, TEXT('x'));
    const FString LargeDisplay = FAgentWorkbenchDisplayFormatter::FormatEventDetail(Large);
    TestTrue(TEXT("Large detail is bounded"), LargeDisplay.Len() < MaxDisplayChars + 100);
    TestTrue(TEXT("Large detail shows truncation"), LargeDisplay.Contains(TEXT("已截断")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentWorkbenchVisibleConversationTest,
    "AgentWorkbench.Display.ConversationMessages",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentWorkbenchVisibleConversationTest::RunTest(const FString& Parameters)
{
    const FGuid FirstRun = FGuid::NewGuid();
    const FGuid SecondRun = FGuid::NewGuid();
    TArray<TSharedPtr<FAgentMessage>> Messages;
    auto AddMessage = [&Messages](EAgentMessageRole Role, const FGuid& RunId, const FString& Text,
        bool bHasToolCall = false)
    {
        TSharedPtr<FAgentMessage> Message = MakeShared<FAgentMessage>();
        Message->Role = Role;
        Message->RunId = RunId;
        Message->Text = Text;
        if (bHasToolCall) { Message->ToolCalls.Add(FAgentToolCall()); }
        Messages.Add(Message);
    };
    AddMessage(EAgentMessageRole::User, FirstRun, TEXT("question one"));
    AddMessage(EAgentMessageRole::Assistant, FirstRun, TEXT("planning"), true);
    AddMessage(EAgentMessageRole::Tool, FirstRun, TEXT("tool result"));
    AddMessage(EAgentMessageRole::Assistant, FirstRun, TEXT("final one"));
    AddMessage(EAgentMessageRole::User, SecondRun, TEXT("question two"));
    AddMessage(EAgentMessageRole::Assistant, SecondRun, TEXT("  "));
    AddMessage(EAgentMessageRole::Error, SecondRun, TEXT("error"));
    AddMessage(EAgentMessageRole::Assistant, FirstRun, TEXT("final one revised"));
    AddMessage(EAgentMessageRole::Assistant, FGuid(), TEXT("legacy final"));
    const TArray<TSharedPtr<FAgentMessage>> Visible =
        FAgentWorkbenchDisplayFormatter::VisibleConversationMessages(Messages);
    TestEqual(TEXT("Users, final answer, and legacy answer are visible"), Visible.Num(), 4);
    if (Visible.Num() == 4)
    {
        TestTrue(TEXT("First entry is user"), Visible[0] == Messages[0]);
        TestTrue(TEXT("Second entry is user without final answer"), Visible[1] == Messages[4]);
        TestTrue(TEXT("Last eligible assistant wins"), Visible[2] == Messages[7]);
        TestTrue(TEXT("Legacy assistant is visible"), Visible[3] == Messages[8]);
    }
    TestEqual(TEXT("Stored messages unchanged"), Messages.Num(), 9);
    return true;
}
#endif
