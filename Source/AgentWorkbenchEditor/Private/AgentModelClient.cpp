#include "AgentModelClient.h"
#include "AgentToolBridge.h"
#include "AgentWorkbenchSession.h"
#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonSerializer.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif

namespace
{
using FJson = TSharedRef<FJsonObject>;

FString EnvironmentValue(const FString& Name)
{
    return Name.IsEmpty() ? FString() : FPlatformMisc::GetEnvironmentVariable(*Name).TrimStartAndEnd();
}

FString Endpoint(EAgentWorkbenchProvider Provider, const FString& Base, const FString& Model)
{
    FString Url = Base;
    if (Url.IsEmpty())
    {
        switch (Provider)
        {
        case EAgentWorkbenchProvider::DeepSeek: Url = TEXT("https://api.deepseek.com"); break;
        case EAgentWorkbenchProvider::OpenAI: Url = TEXT("https://api.openai.com/v1"); break;
        case EAgentWorkbenchProvider::Anthropic: Url = TEXT("https://api.anthropic.com/v1"); break;
        case EAgentWorkbenchProvider::Gemini: Url = TEXT("https://generativelanguage.googleapis.com/v1beta"); break;
        case EAgentWorkbenchProvider::OpenRouter: Url = TEXT("https://openrouter.ai/api/v1"); break;
        case EAgentWorkbenchProvider::Ollama: Url = TEXT("http://127.0.0.1:11434/v1"); break;
        default: return FString();
        }
    }
    Url.RemoveFromEnd(TEXT("/"));
    if (Provider == EAgentWorkbenchProvider::Gemini)
    {
        if (Url.Contains(TEXT(":generateContent"))) { return Url; }
        return Url + TEXT("/models/") + Model + TEXT(":generateContent");
    }
    if (Provider == EAgentWorkbenchProvider::Anthropic)
    {
        return Url.EndsWith(TEXT("/messages")) ? Url : Url + TEXT("/messages");
    }
    return Url.EndsWith(TEXT("/chat/completions")) ? Url : Url + TEXT("/chat/completions");
}

FString SerializeJson(const FJson& Object)
{
    FString Result;
    FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Result));
    return Result;
}

bool ParseObject(const FString& Text, TSharedPtr<FJsonObject>& OutObject)
{
    OutObject.Reset();
    return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), OutObject) && OutObject.IsValid();
}

TSharedPtr<FJsonObject> ObjectField(const TSharedPtr<FJsonObject>& Parent, const TCHAR* Name)
{
    const TSharedPtr<FJsonObject>* Value = nullptr;
    return Parent && Parent->TryGetObjectField(Name, Value) && Value ? *Value : nullptr;
}

TSharedPtr<FJsonObject> ObjectValue(const TSharedPtr<FJsonValue>& Value)
{
    const TSharedPtr<FJsonObject>* Object = nullptr;
    return Value && Value->TryGetObject(Object) && Object ? *Object : nullptr;
}

bool IsSyntheticGeminiId(const FString& Id)
{
    return Id.StartsWith(TEXT("gemini_"), ESearchCase::CaseSensitive);
}

TSharedRef<FJsonObject> GeminiResultObject(const FString& ResultText)
{
    TSharedPtr<FJsonObject> Parsed;
    if (ParseObject(ResultText, Parsed)) { return Parsed.ToSharedRef(); }
    FJson Fallback = MakeShared<FJsonObject>();
    Fallback->SetStringField(TEXT("result"), ResultText);
    return Fallback;
}

FString UserContent(const FAgentConversationTurn& Turn)
{
    if (Turn.IncludedAssets.IsEmpty()) { return Turn.Text; }
    FJson Context = MakeShared<FJsonObject>();
    Context->SetStringField(TEXT("context_type"), TEXT("ue_asset_candidates"));
    TArray<TSharedPtr<FJsonValue>> Assets;
    for (const FAgentCandidate& Asset : Turn.IncludedAssets)
    {
        FJson Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("object_path"), Asset.ObjectPath);
        Entry->SetStringField(TEXT("package_name"), Asset.PackageName);
        Entry->SetStringField(TEXT("asset_class_path"), Asset.AssetClassPath);
        Assets.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Context->SetArrayField(TEXT("assets"), MoveTemp(Assets));
    return Turn.Text + TEXT("\n\nUE asset candidates (object paths only):\n") + SerializeJson(Context);
}

bool BuildBody(EAgentWorkbenchProvider Provider, const FAgentRunInputSnapshot& Snapshot,
    const TArray<FAgentConversationTurn>& Conversation, const TArray<FMCPToolDefinition>& Tools,
    TSharedPtr<FJsonObject>& OutBody, FString& OutError)
{
    FJson Body = MakeShared<FJsonObject>();
    if (Provider == EAgentWorkbenchProvider::Gemini)
    {
        TArray<TSharedPtr<FJsonValue>> Contents;
        for (int32 Index = 0; Index < Conversation.Num(); ++Index)
        {
            const FAgentConversationTurn& Turn = Conversation[Index];
            FJson Item = MakeShared<FJsonObject>();
            TArray<TSharedPtr<FJsonValue>> Parts;
            if (Turn.Role == EAgentMessageRole::Tool)
            {
                Item->SetStringField(TEXT("role"), TEXT("user"));
                FJson Part = MakeShared<FJsonObject>();
                FJson Response = MakeShared<FJsonObject>();
                Response->SetStringField(TEXT("name"), FAgentToolBridge::ToModelName(Turn.ToolName));
                if (!IsSyntheticGeminiId(Turn.ToolCallId))
                {
                    Response->SetStringField(TEXT("id"), Turn.ToolCallId);
                }
                Response->SetObjectField(TEXT("response"), GeminiResultObject(Turn.Text));
                Part->SetObjectField(TEXT("functionResponse"), Response);
                Parts.Add(MakeShared<FJsonValueObject>(Part));
                while (Index + 1 < Conversation.Num() && Conversation[Index + 1].Role == EAgentMessageRole::Tool)
                {
                    const FAgentConversationTurn& Next = Conversation[++Index];
                    FJson NextPart = MakeShared<FJsonObject>();
                    FJson NextResponse = MakeShared<FJsonObject>();
                    NextResponse->SetStringField(TEXT("name"), FAgentToolBridge::ToModelName(Next.ToolName));
                    if (!IsSyntheticGeminiId(Next.ToolCallId))
                    {
                        NextResponse->SetStringField(TEXT("id"), Next.ToolCallId);
                    }
                    NextResponse->SetObjectField(TEXT("response"), GeminiResultObject(Next.Text));
                    NextPart->SetObjectField(TEXT("functionResponse"), NextResponse);
                    Parts.Add(MakeShared<FJsonValueObject>(NextPart));
                }
            }
            else if (Turn.Role == EAgentMessageRole::Assistant)
            {
                Item->SetStringField(TEXT("role"), TEXT("model"));
                if (!Turn.Text.IsEmpty())
                {
                    FJson Part = MakeShared<FJsonObject>();
                    Part->SetStringField(TEXT("text"), Turn.Text);
                    Parts.Add(MakeShared<FJsonValueObject>(Part));
                }
                for (const FAgentToolCall& Call : Turn.ToolCalls)
                {
                    TSharedPtr<FJsonObject> Arguments;
                    if (!ParseObject(Call.ArgumentsJson, Arguments))
                    {
                        OutError = TEXT("历史工具参数不是 JSON 对象，无法继续 Gemini 对话。");
                        return false;
                    }
                    FJson Part = MakeShared<FJsonObject>();
                    FJson Function = MakeShared<FJsonObject>();
                    Function->SetStringField(TEXT("name"), FAgentToolBridge::ToModelName(Call.Name));
                    if (!IsSyntheticGeminiId(Call.Id))
                    {
                        Function->SetStringField(TEXT("id"), Call.Id);
                    }
                    Function->SetObjectField(TEXT("args"), Arguments);
                    Part->SetObjectField(TEXT("functionCall"), Function);
                    if (!Call.ThoughtSignature.IsEmpty())
                    {
                        Part->SetStringField(TEXT("thoughtSignature"), Call.ThoughtSignature);
                    }
                    Parts.Add(MakeShared<FJsonValueObject>(Part));
                }
            }
            else
            {
                Item->SetStringField(TEXT("role"), TEXT("user"));
                FJson Part = MakeShared<FJsonObject>();
                Part->SetStringField(TEXT("text"), UserContent(Turn));
                Parts.Add(MakeShared<FJsonValueObject>(Part));
            }
            Item->SetArrayField(TEXT("parts"), MoveTemp(Parts));
            Contents.Add(MakeShared<FJsonValueObject>(Item));
        }
        Body->SetArrayField(TEXT("contents"), MoveTemp(Contents));
        FJson Config = MakeShared<FJsonObject>();
        Config->SetNumberField(TEXT("maxOutputTokens"), Snapshot.ModelOptions.MaxOutputTokens);
        Body->SetObjectField(TEXT("generationConfig"), Config);
        if (!Tools.IsEmpty())
        {
            TArray<TSharedPtr<FJsonValue>> Declarations;
            for (const FMCPToolDefinition& Tool : Tools)
            {
                FJson Declaration = MakeShared<FJsonObject>();
                Declaration->SetStringField(TEXT("name"), FAgentToolBridge::ToModelName(Tool.Name));
                Declaration->SetStringField(TEXT("description"), Tool.Description);
                // Gemini's parameters Schema omits additionalProperties; the JSON Schema
                // variant accepts the registry's schema without weakening its constraints.
                Declaration->SetObjectField(TEXT("parametersJsonSchema"), Tool.InputSchema);
                Declarations.Add(MakeShared<FJsonValueObject>(Declaration));
            }
            FJson ToolGroup = MakeShared<FJsonObject>();
            ToolGroup->SetArrayField(TEXT("functionDeclarations"), MoveTemp(Declarations));
            Body->SetArrayField(TEXT("tools"), {MakeShared<FJsonValueObject>(ToolGroup)});
            FJson CallingConfig = MakeShared<FJsonObject>();
            CallingConfig->SetStringField(TEXT("mode"), TEXT("AUTO"));
            FJson ToolConfig = MakeShared<FJsonObject>();
            ToolConfig->SetObjectField(TEXT("functionCallingConfig"), CallingConfig);
            Body->SetObjectField(TEXT("toolConfig"), ToolConfig);
        }
        OutBody = Body;
        return true;
    }

    TArray<TSharedPtr<FJsonValue>> Messages;
    for (int32 Index = 0; Index < Conversation.Num(); ++Index)
    {
        const FAgentConversationTurn& Turn = Conversation[Index];
        FJson Item = MakeShared<FJsonObject>();
        if (Provider == EAgentWorkbenchProvider::Anthropic)
        {
            if (Turn.Role == EAgentMessageRole::Tool)
            {
                Item->SetStringField(TEXT("role"), TEXT("user"));
                TArray<TSharedPtr<FJsonValue>> Blocks;
                do
                {
                    const FAgentConversationTurn& Result = Conversation[Index];
                    if (Result.ToolCallId.IsEmpty())
                    {
                        OutError = TEXT("历史工具结果缺少调用 ID，无法继续 Anthropic 对话。");
                        return false;
                    }
                    FJson Block = MakeShared<FJsonObject>();
                    Block->SetStringField(TEXT("type"), TEXT("tool_result"));
                    Block->SetStringField(TEXT("tool_use_id"), Result.ToolCallId);
                    Block->SetStringField(TEXT("content"), Result.Text);
                    Blocks.Add(MakeShared<FJsonValueObject>(Block));
                    ++Index;
                } while (Index < Conversation.Num() && Conversation[Index].Role == EAgentMessageRole::Tool);
                --Index;
                Item->SetArrayField(TEXT("content"), MoveTemp(Blocks));
            }
            else if (Turn.Role == EAgentMessageRole::Assistant && !Turn.ToolCalls.IsEmpty())
            {
                Item->SetStringField(TEXT("role"), TEXT("assistant"));
                TArray<TSharedPtr<FJsonValue>> Blocks;
                if (!Turn.Text.IsEmpty())
                {
                    FJson TextBlock = MakeShared<FJsonObject>();
                    TextBlock->SetStringField(TEXT("type"), TEXT("text"));
                    TextBlock->SetStringField(TEXT("text"), Turn.Text);
                    Blocks.Add(MakeShared<FJsonValueObject>(TextBlock));
                }
                for (const FAgentToolCall& Call : Turn.ToolCalls)
                {
                    TSharedPtr<FJsonObject> Arguments;
                    if (Call.Id.IsEmpty() || !ParseObject(Call.ArgumentsJson, Arguments))
                    {
                        OutError = TEXT("历史工具调用缺少 ID 或参数不是 JSON 对象，无法继续 Anthropic 对话。");
                        return false;
                    }
                    FJson Block = MakeShared<FJsonObject>();
                    Block->SetStringField(TEXT("type"), TEXT("tool_use"));
                    Block->SetStringField(TEXT("id"), Call.Id);
                    Block->SetStringField(TEXT("name"), FAgentToolBridge::ToModelName(Call.Name));
                    Block->SetObjectField(TEXT("input"), Arguments);
                    Blocks.Add(MakeShared<FJsonValueObject>(Block));
                }
                Item->SetArrayField(TEXT("content"), MoveTemp(Blocks));
            }
            else
            {
                Item->SetStringField(TEXT("role"), Turn.Role == EAgentMessageRole::Assistant ? TEXT("assistant") : TEXT("user"));
                Item->SetStringField(TEXT("content"), Turn.Role == EAgentMessageRole::User ? UserContent(Turn) : Turn.Text);
            }
        }
        else if (Turn.Role == EAgentMessageRole::Tool)
        {
            if (Turn.ToolCallId.IsEmpty())
            {
                OutError = TEXT("历史工具结果缺少调用 ID，无法继续模型对话。");
                return false;
            }
            Item->SetStringField(TEXT("role"), TEXT("tool"));
            Item->SetStringField(TEXT("tool_call_id"), Turn.ToolCallId);
            Item->SetStringField(TEXT("content"), Turn.Text);
        }
        else if (Turn.Role == EAgentMessageRole::Assistant && !Turn.ToolCalls.IsEmpty())
        {
            Item->SetStringField(TEXT("role"), TEXT("assistant"));
            Item->SetStringField(TEXT("content"), Turn.Text);
            TArray<TSharedPtr<FJsonValue>> Calls;
            for (const FAgentToolCall& Call : Turn.ToolCalls)
            {
                TSharedPtr<FJsonObject> Arguments;
                if (Call.Id.IsEmpty() || !ParseObject(Call.ArgumentsJson, Arguments))
                {
                    OutError = TEXT("历史工具调用缺少 ID 或参数不是 JSON 对象，无法继续模型对话。");
                    return false;
                }
                FJson Entry = MakeShared<FJsonObject>();
                Entry->SetStringField(TEXT("id"), Call.Id);
                Entry->SetStringField(TEXT("type"), TEXT("function"));
                FJson Function = MakeShared<FJsonObject>();
                Function->SetStringField(TEXT("name"), FAgentToolBridge::ToModelName(Call.Name));
                Function->SetStringField(TEXT("arguments"), Call.ArgumentsJson);
                Entry->SetObjectField(TEXT("function"), Function);
                Calls.Add(MakeShared<FJsonValueObject>(Entry));
            }
            Item->SetArrayField(TEXT("tool_calls"), MoveTemp(Calls));
        }
        else
        {
            Item->SetStringField(TEXT("role"), Turn.Role == EAgentMessageRole::Assistant ? TEXT("assistant") : TEXT("user"));
            Item->SetStringField(TEXT("content"), Turn.Role == EAgentMessageRole::User ? UserContent(Turn) : Turn.Text);
        }
        Messages.Add(MakeShared<FJsonValueObject>(Item));
    }
    Body->SetArrayField(TEXT("messages"), MoveTemp(Messages));
    Body->SetStringField(TEXT("model"), Snapshot.ModelOptions.Model);
    Body->SetNumberField(Provider == EAgentWorkbenchProvider::OpenAI
        ? TEXT("max_completion_tokens") : TEXT("max_tokens"), Snapshot.ModelOptions.MaxOutputTokens);
    if (!Tools.IsEmpty())
    {
        TArray<TSharedPtr<FJsonValue>> Definitions;
        for (const FMCPToolDefinition& Tool : Tools)
        {
            FJson Definition = MakeShared<FJsonObject>();
            if (Provider == EAgentWorkbenchProvider::Anthropic)
            {
                Definition->SetStringField(TEXT("name"), FAgentToolBridge::ToModelName(Tool.Name));
                Definition->SetStringField(TEXT("description"), Tool.Description);
                Definition->SetObjectField(TEXT("input_schema"), Tool.InputSchema);
            }
            else
            {
                Definition->SetStringField(TEXT("type"), TEXT("function"));
                FJson Function = MakeShared<FJsonObject>();
                Function->SetStringField(TEXT("name"), FAgentToolBridge::ToModelName(Tool.Name));
                Function->SetStringField(TEXT("description"), Tool.Description);
                Function->SetObjectField(TEXT("parameters"), Tool.InputSchema);
                Definition->SetObjectField(TEXT("function"), Function);
            }
            Definitions.Add(MakeShared<FJsonValueObject>(Definition));
        }
        Body->SetArrayField(TEXT("tools"), MoveTemp(Definitions));
        if (Provider == EAgentWorkbenchProvider::Anthropic)
        {
            FJson Choice = MakeShared<FJsonObject>();
            Choice->SetStringField(TEXT("type"), TEXT("auto"));
            Body->SetObjectField(TEXT("tool_choice"), Choice);
        }
        else { Body->SetStringField(TEXT("tool_choice"), TEXT("auto")); }
    }
    OutBody = Body;
    return true;
}

bool AddToolCall(const FString& Id, const FString& ModelName, const FString& ArgumentsJson,
    const TMap<FString, FString>& Aliases, TSet<FString>& SeenIds,
    TArray<FAgentToolCall>& OutCalls, FString& OutError)
{
    const FString* RegistryName = Aliases.Find(ModelName);
    if (!RegistryName)
    {
        OutError = FString::Printf(TEXT("模型请求了未授权工具：%s"), *ModelName);
        return false;
    }
    if (Id.IsEmpty() || SeenIds.Contains(Id))
    {
        OutError = TEXT("模型工具调用缺少唯一调用 ID。");
        return false;
    }
    TSharedPtr<FJsonObject> Arguments;
    if (!ParseObject(ArgumentsJson, Arguments))
    {
        OutError = FString::Printf(TEXT("工具 %s 的参数不是 JSON 对象。"), **RegistryName);
        return false;
    }
    SeenIds.Add(Id);
    FAgentToolCall Call;
    Call.Id = Id;
    Call.Name = *RegistryName;
    Call.ArgumentsJson = ArgumentsJson;
    OutCalls.Add(MoveTemp(Call));
    return true;
}

bool ParseResponse(EAgentWorkbenchProvider Provider, const FString& ResponseText, const FGuid& RequestId,
    const TMap<FString, FString>& Aliases, FString& OutText, TArray<FAgentToolCall>& OutCalls, FString& OutError)
{
    TSharedPtr<FJsonObject> Root;
    if (!ParseObject(ResponseText, Root))
    {
        OutError = TEXT("模型返回的内容不是 JSON 对象。");
        return false;
    }
    TSet<FString> SeenIds;
    const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
    if (Provider == EAgentWorkbenchProvider::Anthropic)
    {
        if (!Root->TryGetArrayField(TEXT("content"), Items))
        {
            OutError = TEXT("Anthropic 响应缺少 content 数组。");
            return false;
        }
        for (const TSharedPtr<FJsonValue>& Value : *Items)
        {
            const TSharedPtr<FJsonObject> Part = ObjectValue(Value);
            FString Type;
            if (!Part || !Part->TryGetStringField(TEXT("type"), Type))
            {
                OutError = TEXT("Anthropic 响应含有无效内容块。");
                return false;
            }
            if (Type == TEXT("text"))
            {
                FString Piece;
                if (Part->TryGetStringField(TEXT("text"), Piece)) { OutText += Piece; }
            }
            else if (Type == TEXT("tool_use"))
            {
                FString Id, Name;
                const TSharedPtr<FJsonObject> Arguments = ObjectField(Part, TEXT("input"));
                if (!Part->TryGetStringField(TEXT("id"), Id)
                    || !Part->TryGetStringField(TEXT("name"), Name) || !Arguments)
                {
                    OutError = TEXT("Anthropic 工具调用缺少 ID、名称或参数对象。");
                    return false;
                }
                if (!AddToolCall(Id, Name, SerializeJson(Arguments.ToSharedRef()),
                    Aliases, SeenIds, OutCalls, OutError)) { return false; }
            }
        }
    }
    else if (Provider == EAgentWorkbenchProvider::Gemini)
    {
        if (!Root->TryGetArrayField(TEXT("candidates"), Items) || Items->IsEmpty())
        {
            OutError = TEXT("Gemini 响应缺少 candidates。");
            return false;
        }
        const TSharedPtr<FJsonObject> Candidate = ObjectValue((*Items)[0]);
        const TSharedPtr<FJsonObject> Content = ObjectField(Candidate, TEXT("content"));
        if (!Content || !Content->TryGetArrayField(TEXT("parts"), Items))
        {
            OutError = TEXT("Gemini 响应缺少内容部分。");
            return false;
        }
        int32 CallIndex = 0;
        for (const TSharedPtr<FJsonValue>& Value : *Items)
        {
            const TSharedPtr<FJsonObject> Part = ObjectValue(Value);
            if (!Part)
            {
                OutError = TEXT("Gemini 响应含有无效内容部分。");
                return false;
            }
            FString Piece;
            if (Part->TryGetStringField(TEXT("text"), Piece)) { OutText += Piece; }
            const TSharedPtr<FJsonObject> Function = ObjectField(Part, TEXT("functionCall"));
            if (Function)
            {
                FString Name, Id;
                if (!Function->TryGetStringField(TEXT("name"), Name))
                {
                    OutError = TEXT("Gemini 工具调用缺少名称。");
                    return false;
                }
                if (!Function->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty())
                {
                    Id = FString::Printf(TEXT("gemini_%s_%d"), *RequestId.ToString(EGuidFormats::Digits), CallIndex);
                }
                const TSharedPtr<FJsonObject> Arguments = ObjectField(Function, TEXT("args"));
                const FString ArgumentsJson = Arguments ? SerializeJson(Arguments.ToSharedRef()) : TEXT("{}");
                if (!AddToolCall(Id, Name, ArgumentsJson, Aliases, SeenIds, OutCalls, OutError)) { return false; }
                Part->TryGetStringField(TEXT("thoughtSignature"), OutCalls.Last().ThoughtSignature);
                ++CallIndex;
            }
        }
    }
    else
    {
        if (!Root->TryGetArrayField(TEXT("choices"), Items) || Items->IsEmpty())
        {
            OutError = TEXT("模型响应缺少 choices。");
            return false;
        }
        const TSharedPtr<FJsonObject> Choice = ObjectValue((*Items)[0]);
        const TSharedPtr<FJsonObject> Message = ObjectField(Choice, TEXT("message"));
        if (!Message)
        {
            OutError = TEXT("模型响应缺少 assistant 消息。");
            return false;
        }
        Message->TryGetStringField(TEXT("content"), OutText);
        const TSharedPtr<FJsonValue>* CallsValue = Message->Values.Find(TEXT("tool_calls"));
        if (CallsValue && *CallsValue && (*CallsValue)->Type != EJson::Null)
        {
            if (!Message->TryGetArrayField(TEXT("tool_calls"), Items))
            {
                OutError = TEXT("模型工具调用字段不是数组。");
                return false;
            }
            for (const TSharedPtr<FJsonValue>& Value : *Items)
            {
                const TSharedPtr<FJsonObject> Call = ObjectValue(Value);
                FString Id, Type, Name, Arguments;
                const TSharedPtr<FJsonObject> Function = ObjectField(Call, TEXT("function"));
                if (!Call || !Call->TryGetStringField(TEXT("id"), Id)
                    || !Call->TryGetStringField(TEXT("type"), Type) || Type != TEXT("function")
                    || !Function || !Function->TryGetStringField(TEXT("name"), Name)
                    || !Function->TryGetStringField(TEXT("arguments"), Arguments))
                {
                    OutError = TEXT("模型返回了无效的 function 工具调用。");
                    return false;
                }
                if (!AddToolCall(Id, Name, Arguments, Aliases, SeenIds, OutCalls, OutError)) { return false; }
            }
        }
    }
    if (OutCalls.IsEmpty() && OutText.TrimStartAndEnd().IsEmpty())
    {
        OutError = TEXT("模型返回成功，但没有文本或工具调用。请检查最大输出 Token 限制。");
        return false;
    }
    return true;
}
}

FString FAgentModelClient::ProviderName(EAgentWorkbenchProvider Provider)
{
    switch (Provider)
    {
    case EAgentWorkbenchProvider::DeepSeek: return TEXT("DeepSeek");
    case EAgentWorkbenchProvider::OpenAI: return TEXT("OpenAI");
    case EAgentWorkbenchProvider::Anthropic: return TEXT("Anthropic");
    case EAgentWorkbenchProvider::Gemini: return TEXT("Google Gemini");
    case EAgentWorkbenchProvider::OpenRouter: return TEXT("OpenRouter");
    case EAgentWorkbenchProvider::Ollama: return TEXT("Ollama");
    default: return TEXT("未配置");
    }
}

bool FAgentModelClient::Validate(const UAgentWorkbenchSettings& Settings, const FString& Model, FString& OutError)
{
    OutError.Empty();
    if (Model.TrimStartAndEnd().IsEmpty() || Model.Equals(TEXT("Mock"), ESearchCase::IgnoreCase))
    { OutError = TEXT("请在模型栏选择真实模型，并在项目设置中配置默认模型。"); return false; }
    if (ProviderName(Settings.ProviderType) == TEXT("未配置"))
    { OutError = TEXT("请在项目设置中选择支持的连接类型。"); return false; }
    if (Settings.ProviderType != EAgentWorkbenchProvider::Ollama && EnvironmentValue(Settings.ApiKeyEnvironmentVariable).IsEmpty())
    { OutError = FString::Printf(TEXT("环境变量 %s 未设置；请在启动编辑器前配置 API Key。"), *Settings.ApiKeyEnvironmentVariable); return false; }
    const FString Base = EnvironmentValue(Settings.BaseUrlEnvironmentVariable);
    if (!Base.IsEmpty() && !Base.StartsWith(TEXT("https://")) && !Base.StartsWith(TEXT("http://")))
    { OutError = TEXT("Base URL 环境变量必须是 http:// 或 https:// 地址。"); return false; }
    return true;
}

bool FAgentModelClient::Start(const TSharedRef<FAgentSession>& Session, const FGuid& RequestId, FString& OutError)
{
    check(IsInGameThread());
    OutError.Empty();
    if (!Session->Runner.LastSnapshot || Session->Runner.CurrentRequestId != RequestId
        || Session->Runner.State != EAgentRunState::Running
        || Session->Runner.Phase != EAgentRunPhase::RequestingModel)
    {
        OutError = TEXT("模型请求不属于当前运行中的任务。");
        return false;
    }
    const UAgentWorkbenchSettings* Settings = GetDefault<UAgentWorkbenchSettings>();
    const FAgentRunInputSnapshot& Snapshot = *Session->Runner.LastSnapshot;
    const EAgentWorkbenchProvider Provider = Snapshot.ProviderType;
    const FString Url = Endpoint(Provider, EnvironmentValue(Settings->BaseUrlEnvironmentVariable), Snapshot.ModelOptions.Model);
    if (Url.IsEmpty()) { OutError = TEXT("无法确定模型请求地址。"); return false; }
    const TArray<FMCPToolDefinition> Tools = FAgentToolBridge::ListAvailable(Snapshot);
    TMap<FString, FString> Aliases;
    for (const FMCPToolDefinition& Tool : Tools)
    {
        const FString Alias = FAgentToolBridge::ToModelName(Tool.Name);
        if (Alias.IsEmpty() || !Tool.InputSchema || Aliases.Contains(Alias))
        {
            OutError = FString::Printf(TEXT("工具 %s 的模型名称冲突或参数定义无效。"), *Tool.Name);
            return false;
        }
        Aliases.Add(Alias, Tool.Name);
    }
    TSharedPtr<FJsonObject> Body;
    if (!BuildBody(Provider, Snapshot, Session->Runner.Conversation, Tools, Body, OutError)) { return false; }
    const double RemainingSeconds = Session->Runner.RunDeadlineSeconds - FPlatformTime::Seconds();
    if (RemainingSeconds <= 0.0)
    {
        OutError = TEXT("任务已超过总超时限制。");
        return false;
    }
    const FString BodyText = SerializeJson(Body.ToSharedRef());
    const FGuid RunId = Snapshot.RunId;
    Session->AddEvent(EAgentEventType::ModelRequestStarted, RunId,
        FString::Printf(TEXT("模型输入 · 请求 %s / %s"), *Snapshot.Provider, *Snapshot.ModelOptions.Model), BodyText);
    Session->Touch();
    TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
    Request->SetURL(Url);
    Request->SetVerb(TEXT("POST"));
    Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
    const FString ApiKey = EnvironmentValue(Settings->ApiKeyEnvironmentVariable);
    if (Provider == EAgentWorkbenchProvider::Anthropic)
    {
        Request->SetHeader(TEXT("x-api-key"), ApiKey);
        Request->SetHeader(TEXT("anthropic-version"), TEXT("2023-06-01"));
    }
    else if (Provider == EAgentWorkbenchProvider::Gemini) { Request->SetHeader(TEXT("x-goog-api-key"), ApiKey); }
    else if (!ApiKey.IsEmpty()) { Request->SetHeader(TEXT("Authorization"), TEXT("Bearer ") + ApiKey); }
    Request->SetContentAsString(BodyText);
    Request->SetTimeout(static_cast<float>(FMath::Min(
        static_cast<double>(Snapshot.RequestTimeoutSeconds), RemainingSeconds)));
    TWeakPtr<FAgentSession> WeakSession = Session;
    Request->OnProcessRequestComplete().BindLambda(
        [WeakSession, RunId, RequestId, Provider, Aliases, ApiKey](FHttpRequestPtr, FHttpResponsePtr Response, bool bSucceeded)
    {
        const int32 Status = Response ? Response->GetResponseCode() : 0;
        const FString ResponseText = Response ? Response->GetContentAsString() : FString();
        AsyncTask(ENamedThreads::GameThread,
            [WeakSession, RunId, RequestId, Provider, Aliases, ApiKey, Status, ResponseText, bSucceeded]()
        {
            const TSharedPtr<FAgentSession> Current = WeakSession.Pin();
            if (!Current || Current->Runner.State != EAgentRunState::Running
                || Current->Runner.Phase != EAgentRunPhase::RequestingModel
                || Current->Runner.CurrentRunId != RunId
                || Current->Runner.CurrentRequestId != RequestId) { return; }
            FString SafeResponse = ResponseText;
            if (!ApiKey.IsEmpty()) { SafeResponse.ReplaceInline(*ApiKey, TEXT("[redacted]")); }
            if (!bSucceeded || Status < 200 || Status >= 300)
            {
                if (!SafeResponse.IsEmpty())
                {
                    Current->AddEvent(EAgentEventType::ModelRequestCompleted, RunId,
                        FString::Printf(TEXT("模型输出 · HTTP %d"), Status), SafeResponse);
                    Current->Touch();
                }
                Current->FailRun(RunId, FString::Printf(TEXT("模型请求失败（HTTP %d）：%s"), Status, *SafeResponse.Left(1000)));
                return;
            }
            FString Reply, ParseError;
            TArray<FAgentToolCall> Calls;
            if (!ParseResponse(Provider, ResponseText, RequestId, Aliases, Reply, Calls, ParseError))
            {
                Current->AddEvent(EAgentEventType::ModelRequestCompleted, RunId,
                    TEXT("模型输出 · 解析失败"), SafeResponse);
                Current->Touch();
                Current->FailRun(RunId, ParseError);
                return;
            }
            Current->OnModelResponse(RunId, RequestId, Reply, Calls, SafeResponse);
        });
    });
    Session->Runner.ActiveRequest = Request;
    if (!Request->ProcessRequest())
    {
        Session->Runner.ActiveRequest.Reset();
        OutError = TEXT("无法启动模型网络请求；请检查地址和网络连接。");
        return false;
    }
    return true;
}

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentModelClientToolProtocolTest,
    "AgentWorkbench.ModelClient.ToolProtocols",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentModelClientToolProtocolTest::RunTest(const FString& Parameters)
{
    const FGuid RequestId = FGuid::NewGuid();
    TMap<FString, FString> Aliases;
    const FString EditorAlias = FAgentToolBridge::ToModelName(TEXT("bsharness.editor_info"));
    Aliases.Add(EditorAlias, TEXT("bsharness.editor_info"));

    FString Reply, Error;
    TArray<FAgentToolCall> Calls;
    const FString OpenAIResponse = FString::Printf(TEXT(R"JSON({"choices":[{"message":{"content":null,"tool_calls":[{"id":"call_1","type":"function","function":{"name":"%s","arguments":"{}"}}]}}]})JSON"), *EditorAlias);
    TestTrue(TEXT("OpenAI-compatible function call parses without text"),
        ParseResponse(EAgentWorkbenchProvider::DeepSeek, OpenAIResponse, RequestId, Aliases, Reply, Calls, Error));
    TestEqual(TEXT("OpenAI-compatible call count"), Calls.Num(), 1);
    if (Calls.Num() == 1)
    {
        TestEqual(TEXT("OpenAI-compatible call ID"), Calls[0].Id, FString(TEXT("call_1")));
        TestEqual(TEXT("Provider alias resolves to registry name"), Calls[0].Name, FString(TEXT("bsharness.editor_info")));
    }

    Reply.Empty();
    Error.Empty();
    Calls.Empty();
    const FString AnthropicResponse = FString::Printf(TEXT(R"JSON({"content":[{"type":"text","text":"Checking."},{"type":"tool_use","id":"toolu_1","name":"%s","input":{}}]})JSON"), *EditorAlias);
    TestTrue(TEXT("Anthropic text and tool_use parse"),
        ParseResponse(EAgentWorkbenchProvider::Anthropic, AnthropicResponse, RequestId, Aliases, Reply, Calls, Error));
    TestEqual(TEXT("Anthropic text"), Reply, FString(TEXT("Checking.")));
    TestEqual(TEXT("Anthropic call count"), Calls.Num(), 1);
    if (Calls.Num() == 1) { TestEqual(TEXT("Anthropic call ID"), Calls[0].Id, FString(TEXT("toolu_1"))); }

    Reply.Empty();
    Error.Empty();
    Calls.Empty();
    const FString GeminiResponse = FString::Printf(TEXT(R"JSON({"candidates":[{"content":{"parts":[{"functionCall":{"name":"%s","args":{}},"thoughtSignature":"signature_1"}]}}]})JSON"), *EditorAlias);
    TestTrue(TEXT("Gemini functionCall parses without text"),
        ParseResponse(EAgentWorkbenchProvider::Gemini, GeminiResponse, RequestId, Aliases, Reply, Calls, Error));
    TestEqual(TEXT("Gemini call count"), Calls.Num(), 1);
    if (Calls.Num() == 1)
    {
        TestTrue(TEXT("Gemini missing provider ID gets internal ID"), Calls[0].Id.StartsWith(TEXT("gemini_")));
        TestEqual(TEXT("Gemini thought signature retained"), Calls[0].ThoughtSignature, FString(TEXT("signature_1")));
    }

    Reply.Empty();
    Error.Empty();
    Calls.Empty();
    const FString UnknownResponse = TEXT(R"JSON({"choices":[{"message":{"content":null,"tool_calls":[{"id":"call_2","type":"function","function":{"name":"bsharness_assets_open","arguments":"{}"}}]}}]})JSON");
    TestFalse(TEXT("Unadvertised alias is rejected"),
        ParseResponse(EAgentWorkbenchProvider::OpenAI, UnknownResponse, RequestId, Aliases, Reply, Calls, Error));
    TestTrue(TEXT("Alias rejection reports reason"), Error.Contains(TEXT("未授权工具")));

    // The model response is authorized against the aliases offered in that exact request,
    // including tools added through MCP configuration after the original bridge shipped.
    const FString NewRegistryName = TEXT("bsharness.blueprint.variables");
    const FString NewAlias = FAgentToolBridge::ToModelName(NewRegistryName);
    Aliases.Add(NewAlias, NewRegistryName);
    const FString ConfiguredResponse = FString::Printf(TEXT(R"JSON({"choices":[{"message":{"content":null,"tool_calls":[{"id":"call_configured","type":"function","function":{"name":"%s","arguments":"{}"}}]}}]})JSON"), *NewAlias);
    Reply.Empty();
    Error.Empty();
    Calls.Empty();
    TestTrue(TEXT("Configured MCP alias parses"),
        ParseResponse(EAgentWorkbenchProvider::OpenAI, ConfiguredResponse, RequestId, Aliases, Reply, Calls, Error));
    TestEqual(TEXT("Configured MCP call count"), Calls.Num(), 1);
    if (Calls.Num() == 1)
    { TestEqual(TEXT("Configured MCP name"), Calls[0].Name, NewRegistryName); }

    FMCPToolDefinition Definition;
    Definition.Name = TEXT("bsharness.editor_info");
    Definition.Description = TEXT("Read editor information");
    Definition.InputSchema = MakeShared<FJsonObject>();
    Definition.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
    Definition.InputSchema->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
    TArray<FMCPToolDefinition> Tools;
    Tools.Add(Definition);

    FAgentRunInputSnapshot Snapshot;
    Snapshot.ModelOptions.Model = TEXT("protocol-test-model");
    FAgentConversationTurn User;
    User.Role = EAgentMessageRole::User;
    User.Text = TEXT("Show editor info");
    FAgentConversationTurn Assistant;
    Assistant.Role = EAgentMessageRole::Assistant;
    FAgentToolCall Call;
    Call.Id = TEXT("call_1");
    Call.Name = Definition.Name;
    Call.ArgumentsJson = TEXT("{}");
    Call.ThoughtSignature = TEXT("signature_1");
    Assistant.ToolCalls.Add(Call);
    FAgentConversationTurn ToolResult;
    ToolResult.Role = EAgentMessageRole::Tool;
    ToolResult.ToolCallId = Call.Id;
    ToolResult.ToolName = Call.Name;
    ToolResult.Text = TEXT("{\"ok\":true}");
    TArray<FAgentConversationTurn> Conversation;
    Conversation.Add(User);
    Conversation.Add(Assistant);
    Conversation.Add(ToolResult);

    TSharedPtr<FJsonObject> Body;
    Error.Empty();
    TestTrue(TEXT("OpenAI-compatible continuation body builds"),
        BuildBody(EAgentWorkbenchProvider::DeepSeek, Snapshot, Conversation, Tools, Body, Error));
    const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
    if (Body && Body->TryGetArrayField(TEXT("messages"), Items) && Items->Num() == 3)
    {
        const TSharedPtr<FJsonObject> ResultMessage = ObjectValue((*Items)[2]);
        FString ToolCallId;
        TestTrue(TEXT("OpenAI-compatible tool result preserves call ID"),
            ResultMessage && ResultMessage->TryGetStringField(TEXT("tool_call_id"), ToolCallId) && ToolCallId == Call.Id);
    }
    else { AddError(TEXT("OpenAI-compatible continuation lost conversation messages.")); }

    Body.Reset();
    Error.Empty();
    TestTrue(TEXT("Anthropic continuation body builds"),
        BuildBody(EAgentWorkbenchProvider::Anthropic, Snapshot, Conversation, Tools, Body, Error));
    if (Body && Body->TryGetArrayField(TEXT("messages"), Items) && Items->Num() == 3)
    {
        const TSharedPtr<FJsonObject> ResultMessage = ObjectValue((*Items)[2]);
        const TArray<TSharedPtr<FJsonValue>>* Blocks = nullptr;
        const TSharedPtr<FJsonObject> Block = ResultMessage
            && ResultMessage->TryGetArrayField(TEXT("content"), Blocks) && Blocks->Num() == 1
                ? ObjectValue((*Blocks)[0]) : nullptr;
        FString ToolUseId;
        TestTrue(TEXT("Anthropic tool_result preserves tool_use_id"),
            Block && Block->TryGetStringField(TEXT("tool_use_id"), ToolUseId) && ToolUseId == Call.Id);
    }
    else { AddError(TEXT("Anthropic continuation lost conversation messages.")); }

    Body.Reset();
    Error.Empty();
    TestTrue(TEXT("Gemini continuation body builds"),
        BuildBody(EAgentWorkbenchProvider::Gemini, Snapshot, Conversation, Tools, Body, Error));
    if (Body && Body->TryGetArrayField(TEXT("contents"), Items) && Items->Num() == 3)
    {
        const TSharedPtr<FJsonObject> ModelMessage = ObjectValue((*Items)[1]);
        const TArray<TSharedPtr<FJsonValue>>* Parts = nullptr;
        const TSharedPtr<FJsonObject> ModelPart = ModelMessage
            && ModelMessage->TryGetArrayField(TEXT("parts"), Parts) && Parts->Num() == 1
                ? ObjectValue((*Parts)[0]) : nullptr;
        FString ThoughtSignature;
        TestTrue(TEXT("Gemini continuation replays thought signature"),
            ModelPart && ModelPart->TryGetStringField(TEXT("thoughtSignature"), ThoughtSignature)
            && ThoughtSignature == Call.ThoughtSignature);
        const TSharedPtr<FJsonObject> ResultMessage = ObjectValue((*Items)[2]);
        const TArray<TSharedPtr<FJsonValue>>* ResultParts = nullptr;
        const TSharedPtr<FJsonObject> ResultPart = ResultMessage
            && ResultMessage->TryGetArrayField(TEXT("parts"), ResultParts) && ResultParts->Num() == 1
                ? ObjectValue((*ResultParts)[0]) : nullptr;
        const TSharedPtr<FJsonObject> FunctionResponse = ObjectField(ResultPart, TEXT("functionResponse"));
        const TSharedPtr<FJsonObject> StructuredResult = ObjectField(FunctionResponse, TEXT("response"));
        bool bOk = false;
        TestTrue(TEXT("Gemini receives structured tool result"),
            StructuredResult && StructuredResult->TryGetBoolField(TEXT("ok"), bOk) && bOk);
        const TArray<TSharedPtr<FJsonValue>>* ToolGroups = nullptr;
        const TSharedPtr<FJsonObject> Group = Body->TryGetArrayField(TEXT("tools"), ToolGroups) && ToolGroups->Num() == 1
            ? ObjectValue((*ToolGroups)[0]) : nullptr;
        const TArray<TSharedPtr<FJsonValue>>* Declarations = nullptr;
        const TSharedPtr<FJsonObject> Declaration = Group
            && Group->TryGetArrayField(TEXT("functionDeclarations"), Declarations) && Declarations->Num() == 1
                ? ObjectValue((*Declarations)[0]) : nullptr;
        TestTrue(TEXT("Gemini receives JSON Schema declaration"),
            ObjectField(Declaration, TEXT("parametersJsonSchema")).IsValid());
    }
    else { AddError(TEXT("Gemini continuation lost conversation contents.")); }
    return true;
}

#endif
