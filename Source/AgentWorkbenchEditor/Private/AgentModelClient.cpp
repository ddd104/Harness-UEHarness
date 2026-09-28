#include "AgentModelClient.h"
#include "AgentWorkbenchSession.h"
#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformMisc.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonSerializer.h"

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
    FString ContextJson;
    FJsonSerializer::Serialize(Context, TJsonWriterFactory<>::Create(&ContextJson));
    return Turn.Text + TEXT("\n\nUE asset candidates (object paths only):\n") + ContextJson;
}

FJson BuildBody(EAgentWorkbenchProvider Provider, const FAgentRunInputSnapshot& Snapshot)
{
    FJson Body = MakeShared<FJsonObject>();
    if (Provider == EAgentWorkbenchProvider::Gemini)
    {
        TArray<TSharedPtr<FJsonValue>> Contents;
        for (const FAgentConversationTurn& Turn : Snapshot.Conversation)
        {
            FJson Item = MakeShared<FJsonObject>();
            Item->SetStringField(TEXT("role"), Turn.Role == EAgentMessageRole::Assistant ? TEXT("model") : TEXT("user"));
            FJson Part = MakeShared<FJsonObject>();
            Part->SetStringField(TEXT("text"), Turn.Role == EAgentMessageRole::User ? UserContent(Turn) : Turn.Text);
            Item->SetArrayField(TEXT("parts"), {MakeShared<FJsonValueObject>(Part)});
            Contents.Add(MakeShared<FJsonValueObject>(Item));
        }
        Body->SetArrayField(TEXT("contents"), MoveTemp(Contents));
        FJson Config = MakeShared<FJsonObject>();
        Config->SetNumberField(TEXT("maxOutputTokens"), Snapshot.ModelOptions.MaxOutputTokens);
        Body->SetObjectField(TEXT("generationConfig"), Config);
        return Body;
    }
    TArray<TSharedPtr<FJsonValue>> Messages;
    for (const FAgentConversationTurn& Turn : Snapshot.Conversation)
    {
        FJson Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("role"), Turn.Role == EAgentMessageRole::Assistant ? TEXT("assistant") : TEXT("user"));
        Item->SetStringField(TEXT("content"), Turn.Role == EAgentMessageRole::User ? UserContent(Turn) : Turn.Text);
        Messages.Add(MakeShared<FJsonValueObject>(Item));
    }
    Body->SetArrayField(TEXT("messages"), MoveTemp(Messages));
    if (Provider != EAgentWorkbenchProvider::Anthropic) { Body->SetStringField(TEXT("model"), Snapshot.ModelOptions.Model); }
    Body->SetNumberField(Provider == EAgentWorkbenchProvider::OpenAI
        ? TEXT("max_completion_tokens") : TEXT("max_tokens"), Snapshot.ModelOptions.MaxOutputTokens);
    if (Provider == EAgentWorkbenchProvider::Anthropic) { Body->SetStringField(TEXT("model"), Snapshot.ModelOptions.Model); }
    return Body;
}

bool ExtractText(EAgentWorkbenchProvider Provider, const FString& ResponseText, FString& OutText)
{
    TSharedPtr<FJsonObject> Root;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ResponseText), Root) || !Root) { return false; }
    const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
    if (Provider == EAgentWorkbenchProvider::Anthropic)
    {
        if (!Root->TryGetArrayField(TEXT("content"), Items)) { return false; }
        for (const auto& Value : *Items)
        {
            const TSharedPtr<FJsonObject> Part = Value ? Value->AsObject() : nullptr;
            FString Text;
            if (Part && Part->TryGetStringField(TEXT("text"), Text)) { OutText += Text; }
        }
        return !OutText.IsEmpty();
    }
    if (Provider == EAgentWorkbenchProvider::Gemini)
    {
        if (!Root->TryGetArrayField(TEXT("candidates"), Items) || Items->IsEmpty()) { return false; }
        const TSharedPtr<FJsonObject> Candidate = (*Items)[0]->AsObject();
        const TSharedPtr<FJsonObject>* Content = nullptr;
        if (!Candidate || !Candidate->TryGetObjectField(TEXT("content"), Content) || !(*Content)->TryGetArrayField(TEXT("parts"), Items)) { return false; }
        for (const auto& Value : *Items)
        {
            const TSharedPtr<FJsonObject> Part = Value ? Value->AsObject() : nullptr;
            FString Text;
            if (Part && Part->TryGetStringField(TEXT("text"), Text)) { OutText += Text; }
        }
        return !OutText.IsEmpty();
    }
    if (!Root->TryGetArrayField(TEXT("choices"), Items) || Items->IsEmpty()) { return false; }
    const TSharedPtr<FJsonObject> Choice = (*Items)[0]->AsObject();
    const TSharedPtr<FJsonObject>* Message = nullptr;
    return Choice && Choice->TryGetObjectField(TEXT("message"), Message)
        && (*Message)->TryGetStringField(TEXT("content"), OutText) && !OutText.IsEmpty();
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

bool FAgentModelClient::Start(const TSharedRef<FAgentSession>& Session, FString& OutError)
{
    check(IsInGameThread());
    const UAgentWorkbenchSettings* Settings = GetDefault<UAgentWorkbenchSettings>();
    const FAgentRunInputSnapshot& Snapshot = *Session->Runner.LastSnapshot;
    const EAgentWorkbenchProvider Provider = Settings->ProviderType;
    const FString Url = Endpoint(Provider, EnvironmentValue(Settings->BaseUrlEnvironmentVariable), Snapshot.ModelOptions.Model);
    if (Url.IsEmpty()) { OutError = TEXT("无法确定模型请求地址。"); return false; }
    const FJson Body = BuildBody(Provider, Snapshot);
    FString BodyText;
    FJsonSerializer::Serialize(Body, TJsonWriterFactory<>::Create(&BodyText));
    const FGuid RunId = Snapshot.RunId;
    Session->AddEvent(EAgentEventType::ModelRequestStarted, RunId,
        FString::Printf(TEXT("请求 %s / %s"), *Snapshot.Provider, *Snapshot.ModelOptions.Model), BodyText);
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
    Request->SetTimeout(static_cast<float>(FMath::Min(Settings->RequestTimeoutSeconds, Settings->RunTimeoutSeconds)));
    TWeakPtr<FAgentSession> WeakSession = Session;
    Request->OnProcessRequestComplete().BindLambda([WeakSession, RunId, Provider](FHttpRequestPtr, FHttpResponsePtr Response, bool bSucceeded)
    {
        const int32 Status = Response ? Response->GetResponseCode() : 0;
        const FString ResponseText = Response ? Response->GetContentAsString() : FString();
        AsyncTask(ENamedThreads::GameThread, [WeakSession, RunId, Provider, Status, ResponseText, bSucceeded]()
        {
            const TSharedPtr<FAgentSession> Current = WeakSession.Pin();
            if (!Current || Current->Runner.State != EAgentRunState::Running || Current->Runner.CurrentRunId != RunId) { return; }
            if (!bSucceeded || Status < 200 || Status >= 300)
            {
                Current->FailRun(RunId, FString::Printf(TEXT("模型请求失败（HTTP %d）：%s"), Status,
                    *ResponseText.Left(1000)));
                return;
            }
            FString Reply;
            if (!ExtractText(Provider, ResponseText, Reply))
            { Current->FailRun(RunId, TEXT("模型返回成功，但没有可显示的文本回复。请检查最大输出 Token 限制。")); return; }
            Current->CompleteRun(RunId, Reply);
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
