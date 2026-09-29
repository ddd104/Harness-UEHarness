#include "AgentWorkbenchHistory.h"
#include "AgentAssetContextService.h"
#include "AgentWorkbenchSettings.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"

namespace
{
using FJson = TSharedRef<FJsonObject>;

FJson NewObject() { return MakeShared<FJsonObject>(); }
TSharedPtr<FJsonValue> Value(const FJson& Object) { return MakeShared<FJsonValueObject>(Object); }

FJson CandidateToJson(const FAgentCandidate& Candidate)
{
    FJson Json = NewObject();
    Json->SetStringField(TEXT("candidate_id"), Candidate.CandidateId.ToString());
    Json->SetStringField(TEXT("display_name"), Candidate.DisplayName);
    Json->SetStringField(TEXT("object_path"), Candidate.ObjectPath);
    Json->SetStringField(TEXT("package_name"), Candidate.PackageName);
    Json->SetStringField(TEXT("asset_class_path"), Candidate.AssetClassPath);
    Json->SetBoolField(TEXT("included"), Candidate.bIncluded);
    Json->SetNumberField(TEXT("validation_status"), static_cast<int32>(Candidate.ValidationStatus));
    Json->SetStringField(TEXT("added_at"), Candidate.AddedAt.ToIso8601());
    return Json;
}

TArray<TSharedPtr<FJsonValue>> CandidateArray(const TArray<FAgentCandidate>& Candidates)
{
    TArray<TSharedPtr<FJsonValue>> Array;
    for (const FAgentCandidate& Candidate : Candidates) { Array.Add(Value(CandidateToJson(Candidate))); }
    return Array;
}

TArray<TSharedPtr<FJsonValue>> ToolCallArray(const TArray<FAgentToolCall>& ToolCalls)
{
    TArray<TSharedPtr<FJsonValue>> Array;
    for (const FAgentToolCall& ToolCall : ToolCalls)
    {
        FJson Item = NewObject();
        Item->SetStringField(TEXT("id"), ToolCall.Id);
        Item->SetStringField(TEXT("name"), ToolCall.Name);
        Item->SetStringField(TEXT("arguments_json"), ToolCall.ArgumentsJson);
        Item->SetStringField(TEXT("thought_signature"), ToolCall.ThoughtSignature);
        Array.Add(Value(Item));
    }
    return Array;
}

FJson TurnToJson(const FAgentConversationTurn& Turn)
{
    FJson Json = NewObject();
    Json->SetNumberField(TEXT("role"), static_cast<int32>(Turn.Role));
    Json->SetStringField(TEXT("text"), Turn.Text);
    Json->SetArrayField(TEXT("included_assets"), CandidateArray(Turn.IncludedAssets));
    Json->SetArrayField(TEXT("tool_calls"), ToolCallArray(Turn.ToolCalls));
    Json->SetStringField(TEXT("tool_call_id"), Turn.ToolCallId);
    Json->SetStringField(TEXT("tool_name"), Turn.ToolName);
    return Json;
}

FJson SnapshotToJson(const FAgentRunInputSnapshot& Snapshot)
{
    FJson Json = NewObject();
    Json->SetStringField(TEXT("session_id"), Snapshot.SessionId.ToString());
    Json->SetStringField(TEXT("run_id"), Snapshot.RunId.ToString());
    Json->SetStringField(TEXT("user_input"), Snapshot.UserInput);
    Json->SetStringField(TEXT("model"), Snapshot.ModelOptions.Model);
    Json->SetStringField(TEXT("provider"), Snapshot.Provider);
    Json->SetNumberField(TEXT("provider_type"), static_cast<int32>(Snapshot.ProviderType));
    Json->SetNumberField(TEXT("max_output_tokens"), Snapshot.ModelOptions.MaxOutputTokens);
    Json->SetNumberField(TEXT("request_timeout_seconds"), Snapshot.RequestTimeoutSeconds);
    Json->SetNumberField(TEXT("tool_timeout_seconds"), Snapshot.ToolTimeoutSeconds);
    Json->SetNumberField(TEXT("run_timeout_seconds"), Snapshot.RunTimeoutSeconds);
    Json->SetNumberField(TEXT("max_tool_steps"), Snapshot.MaxToolSteps);
    Json->SetStringField(TEXT("created_at"), Snapshot.CreatedAt.ToIso8601());
    Json->SetArrayField(TEXT("included_assets"), CandidateArray(Snapshot.IncludedAssets));
    // Tool authorizations are only used by the active Run. Historical Runs never resume
    // execution, and the actual calls and results are recorded in messages and events.
    Json->SetBoolField(TEXT("tool_list_frozen"), Snapshot.bToolListFrozen);
    TArray<TSharedPtr<FJsonValue>> Conversation;
    for (const FAgentConversationTurn& Turn : Snapshot.Conversation) { Conversation.Add(Value(TurnToJson(Turn))); }
    Json->SetArrayField(TEXT("conversation"), MoveTemp(Conversation));
    return Json;
}

bool ReadDate(const FJsonObject& Json, const TCHAR* Name, FDateTime& Out)
{
    FString String;
    return Json.TryGetStringField(Name, String) && FDateTime::ParseIso8601(*String, Out);
}

bool ReadGuid(const FJsonObject& Json, const TCHAR* Name, FGuid& Out)
{
    FString String;
    return Json.TryGetStringField(Name, String) && FGuid::Parse(String, Out) && Out.IsValid();
}

bool ReadCandidate(const FJsonObject& Json, FAgentCandidate& Out)
{
    if (!ReadGuid(Json, TEXT("candidate_id"), Out.CandidateId)
        || !Json.TryGetStringField(TEXT("display_name"), Out.DisplayName)
        || !Json.TryGetStringField(TEXT("object_path"), Out.ObjectPath)
        || !Json.TryGetStringField(TEXT("package_name"), Out.PackageName)
        || !Json.TryGetStringField(TEXT("asset_class_path"), Out.AssetClassPath)
        || !Json.TryGetBoolField(TEXT("included"), Out.bIncluded)) { return false; }
    int32 Status = 0;
    if (Json.TryGetNumberField(TEXT("validation_status"), Status)
        && Status >= 0 && Status <= static_cast<int32>(EAgentCandidateValidation::Unsupported))
    { Out.ValidationStatus = static_cast<EAgentCandidateValidation>(Status); }
    ReadDate(Json, TEXT("added_at"), Out.AddedAt);
    return true;
}

void ReadCandidates(const FJsonObject& Json, const TCHAR* Name, TArray<FAgentCandidate>& Out)
{
    const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
    if (!Json.TryGetArrayField(Name, Array)) { return; }
    for (const TSharedPtr<FJsonValue>& Entry : *Array)
    {
        const TSharedPtr<FJsonObject> Object = Entry ? Entry->AsObject() : nullptr;
        FAgentCandidate Candidate;
        if (Object && ReadCandidate(*Object, Candidate)) { Out.Add(MoveTemp(Candidate)); }
    }
}

void ReadToolCalls(const FJsonObject& Json, const TCHAR* Name, TArray<FAgentToolCall>& Out)
{
    const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
    if (!Json.TryGetArrayField(Name, Array)) { return; }
    for (const TSharedPtr<FJsonValue>& Entry : *Array)
    {
        const TSharedPtr<FJsonObject> Item = Entry ? Entry->AsObject() : nullptr;
        if (!Item) { continue; }
        FAgentToolCall ToolCall;
        if (Item->TryGetStringField(TEXT("id"), ToolCall.Id)
            && Item->TryGetStringField(TEXT("name"), ToolCall.Name)
            && Item->TryGetStringField(TEXT("arguments_json"), ToolCall.ArgumentsJson))
        {
            Item->TryGetStringField(TEXT("thought_signature"), ToolCall.ThoughtSignature);
            Out.Add(MoveTemp(ToolCall));
        }
    }
}

void ReadConversation(const FJsonObject& Json, TArray<FAgentConversationTurn>& Out)
{
    const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
    if (!Json.TryGetArrayField(TEXT("conversation"), Array)) { return; }
    for (const TSharedPtr<FJsonValue>& Entry : *Array)
    {
        const TSharedPtr<FJsonObject> Item = Entry ? Entry->AsObject() : nullptr;
        if (!Item) { continue; }
        FAgentConversationTurn Turn;
        int32 Role = 0;
        if (!Item->TryGetNumberField(TEXT("role"), Role)
            || Role < 0 || Role > static_cast<int32>(EAgentMessageRole::Error)
            || !Item->TryGetStringField(TEXT("text"), Turn.Text)) { continue; }
        Turn.Role = static_cast<EAgentMessageRole>(Role);
        ReadCandidates(*Item, TEXT("included_assets"), Turn.IncludedAssets);
        ReadToolCalls(*Item, TEXT("tool_calls"), Turn.ToolCalls);
        Item->TryGetStringField(TEXT("tool_call_id"), Turn.ToolCallId);
        Item->TryGetStringField(TEXT("tool_name"), Turn.ToolName);
        Out.Add(MoveTemp(Turn));
    }
}

EAgentWorkbenchProvider ProviderFromName(const FString& Name)
{
    if (Name == TEXT("OpenAI")) { return EAgentWorkbenchProvider::OpenAI; }
    if (Name == TEXT("Anthropic")) { return EAgentWorkbenchProvider::Anthropic; }
    if (Name == TEXT("Google Gemini")) { return EAgentWorkbenchProvider::Gemini; }
    if (Name == TEXT("OpenRouter")) { return EAgentWorkbenchProvider::OpenRouter; }
    if (Name == TEXT("Ollama")) { return EAgentWorkbenchProvider::Ollama; }
    return EAgentWorkbenchProvider::DeepSeek;
}

bool ReadSnapshot(const FJsonObject& Json, FAgentRunInputSnapshot& Out)
{
    if (!ReadGuid(Json, TEXT("session_id"), Out.SessionId)
        || !ReadGuid(Json, TEXT("run_id"), Out.RunId)
        || !Json.TryGetStringField(TEXT("user_input"), Out.UserInput)
        || !Json.TryGetStringField(TEXT("model"), Out.ModelOptions.Model)) { return false; }
    Json.TryGetNumberField(TEXT("max_output_tokens"), Out.ModelOptions.MaxOutputTokens);
    Json.TryGetStringField(TEXT("provider"), Out.Provider);
    Out.ProviderType = ProviderFromName(Out.Provider);
    int32 ProviderType = 0;
    if (Json.TryGetNumberField(TEXT("provider_type"), ProviderType)
        && ProviderType >= 0 && ProviderType <= static_cast<int32>(EAgentWorkbenchProvider::Ollama))
    { Out.ProviderType = static_cast<EAgentWorkbenchProvider>(ProviderType); }
    int32 Limit = 0;
    if (Json.TryGetNumberField(TEXT("request_timeout_seconds"), Limit) && Limit > 0)
    { Out.RequestTimeoutSeconds = Limit; }
    if (Json.TryGetNumberField(TEXT("tool_timeout_seconds"), Limit) && Limit > 0)
    { Out.ToolTimeoutSeconds = Limit; }
    if (Json.TryGetNumberField(TEXT("run_timeout_seconds"), Limit) && Limit > 0)
    { Out.RunTimeoutSeconds = Limit; }
    if (Json.TryGetNumberField(TEXT("max_tool_steps"), Limit) && Limit > 0)
    { Out.MaxToolSteps = Limit; }
    ReadDate(Json, TEXT("created_at"), Out.CreatedAt);
    ReadCandidates(Json, TEXT("included_assets"), Out.IncludedAssets);
    const TArray<TSharedPtr<FJsonValue>>* AllowedToolNames = nullptr;
    if (Json.TryGetArrayField(TEXT("allowed_tool_names"), AllowedToolNames))
    {
        for (const TSharedPtr<FJsonValue>& Entry : *AllowedToolNames)
        {
            FString Name;
            if (Entry && Entry->TryGetString(Name)) { Out.AllowedToolNames.Add(MoveTemp(Name)); }
        }
    }
    const TSharedPtr<FJsonObject>* ToolHandlerIds = nullptr;
    if (Json.TryGetObjectField(TEXT("allowed_tool_handler_ids"), ToolHandlerIds) && ToolHandlerIds)
    {
        for (const auto& Entry : (*ToolHandlerIds)->Values)
        {
            FString HandlerId;
            if (Entry.Value && Entry.Value->TryGetString(HandlerId))
            { Out.AllowedToolHandlerIds.Add(FString(Entry.Key), MoveTemp(HandlerId)); }
        }
    }
    Json.TryGetBoolField(TEXT("tool_list_frozen"), Out.bToolListFrozen);
    ReadConversation(Json, Out.Conversation);
    return true;
}

FJson SessionToJson(const FAgentSession& Session)
{
    FJson Json = NewObject();
    Json->SetNumberField(TEXT("version"), 2);
    Json->SetStringField(TEXT("session_id"), Session.SessionId.ToString());
    Json->SetStringField(TEXT("title"), Session.Title);
    Json->SetBoolField(TEXT("title_manually_set"), Session.bTitleManuallySet);
    Json->SetStringField(TEXT("created_at"), Session.CreatedAt.ToIso8601());
    Json->SetStringField(TEXT("updated_at"), Session.UpdatedAt.ToIso8601());
    Json->SetStringField(TEXT("model"), Session.ModelOptions.Model);
    Json->SetNumberField(TEXT("max_output_tokens"), Session.ModelOptions.MaxOutputTokens);
    Json->SetStringField(TEXT("draft"), Session.DraftText);
    Json->SetNumberField(TEXT("run_state"), static_cast<int32>(Session.Runner.State));
    TArray<TSharedPtr<FJsonValue>> Candidates;
    for (const TSharedPtr<FAgentCandidate>& Candidate : Session.Candidates)
    { if (Candidate) { Candidates.Add(Value(CandidateToJson(*Candidate))); } }
    Json->SetArrayField(TEXT("candidates"), MoveTemp(Candidates));
    TArray<TSharedPtr<FJsonValue>> Messages;
    for (const TSharedPtr<FAgentMessage>& Message : Session.Messages)
    {
        if (!Message) { continue; }
        FJson Item = NewObject();
        Item->SetStringField(TEXT("message_id"), Message->MessageId.ToString());
        Item->SetNumberField(TEXT("role"), static_cast<int32>(Message->Role));
        Item->SetStringField(TEXT("text"), Message->Text);
        Item->SetStringField(TEXT("run_id"), Message->RunId.ToString());
        Item->SetArrayField(TEXT("included_assets"), CandidateArray(Message->IncludedAssets));
        Item->SetArrayField(TEXT("tool_calls"), ToolCallArray(Message->ToolCalls));
        Item->SetStringField(TEXT("tool_call_id"), Message->ToolCallId);
        Item->SetStringField(TEXT("tool_name"), Message->ToolName);
        Messages.Add(Value(Item));
    }
    Json->SetArrayField(TEXT("messages"), MoveTemp(Messages));
    TArray<TSharedPtr<FJsonValue>> Events;
    for (const TSharedPtr<FAgentEvent>& Event : Session.Events)
    {
        if (!Event) { continue; }
        FJson Item = NewObject();
        Item->SetStringField(TEXT("run_id"), Event->RunId.ToString());
        Item->SetNumberField(TEXT("sequence"), Event->Sequence);
        Item->SetNumberField(TEXT("type"), static_cast<int32>(Event->Type));
        Item->SetStringField(TEXT("time"), Event->Time.ToIso8601());
        Item->SetStringField(TEXT("summary"), Event->Summary);
        Item->SetStringField(TEXT("detail"), GetDefault<UAgentWorkbenchSettings>()->bPersistDetailedPayloads
            ? Event->Detail : FString());
        Events.Add(Value(Item));
    }
    Json->SetArrayField(TEXT("events"), MoveTemp(Events));
    TArray<TSharedPtr<FJsonValue>> Runs;
    for (const FAgentRunInputSnapshot& Snapshot : Session.RunHistory) { Runs.Add(Value(SnapshotToJson(Snapshot))); }
    Json->SetArrayField(TEXT("run_history"), MoveTemp(Runs));
    return Json;
}

bool SessionFromJson(const FJsonObject& Json, FAgentSession& Session)
{
    int32 Version = 0;
    if (!Json.TryGetNumberField(TEXT("version"), Version) || (Version != 1 && Version != 2)
        || !ReadGuid(Json, TEXT("session_id"), Session.SessionId)
        || !Json.TryGetStringField(TEXT("title"), Session.Title)) { return false; }
    ReadDate(Json, TEXT("created_at"), Session.CreatedAt);
    Json.TryGetBoolField(TEXT("title_manually_set"), Session.bTitleManuallySet);
    ReadDate(Json, TEXT("updated_at"), Session.UpdatedAt);
    Json.TryGetStringField(TEXT("model"), Session.ModelOptions.Model);
    Json.TryGetNumberField(TEXT("max_output_tokens"), Session.ModelOptions.MaxOutputTokens);
    Json.TryGetStringField(TEXT("draft"), Session.DraftText);
    int32 State = 0;
    Json.TryGetNumberField(TEXT("run_state"), State);
    Session.Runner.State = State == static_cast<int32>(EAgentRunState::Running)
        ? EAgentRunState::Interrupted
        : (State >= 0 && State <= static_cast<int32>(EAgentRunState::Interrupted)
            ? static_cast<EAgentRunState>(State) : EAgentRunState::Idle);
    TArray<FAgentCandidate> Candidates;
    ReadCandidates(Json, TEXT("candidates"), Candidates);
    for (FAgentCandidate& Candidate : Candidates)
    { Session.Candidates.Add(MakeShared<FAgentCandidate>(MoveTemp(Candidate))); }
    const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
    if (Json.TryGetArrayField(TEXT("messages"), Array))
    {
        for (const TSharedPtr<FJsonValue>& Entry : *Array)
        {
            const TSharedPtr<FJsonObject> Item = Entry ? Entry->AsObject() : nullptr;
            if (!Item) { continue; }
            TSharedPtr<FAgentMessage> Message = MakeShared<FAgentMessage>();
            int32 Role = 0;
            if (!ReadGuid(*Item, TEXT("message_id"), Message->MessageId)
                || !Item->TryGetStringField(TEXT("text"), Message->Text)
                || !Item->TryGetNumberField(TEXT("role"), Role)
                || Role < 0 || Role > static_cast<int32>(EAgentMessageRole::Error)) { continue; }
            ReadGuid(*Item, TEXT("run_id"), Message->RunId);
            Message->Role = static_cast<EAgentMessageRole>(Role);
            ReadCandidates(*Item, TEXT("included_assets"), Message->IncludedAssets);
            ReadToolCalls(*Item, TEXT("tool_calls"), Message->ToolCalls);
            Item->TryGetStringField(TEXT("tool_call_id"), Message->ToolCallId);
            Item->TryGetStringField(TEXT("tool_name"), Message->ToolName);
            Session.Messages.Add(Message);
        }
    }
    if (Json.TryGetArrayField(TEXT("events"), Array))
    {
        for (const TSharedPtr<FJsonValue>& Entry : *Array)
        {
            const TSharedPtr<FJsonObject> Item = Entry ? Entry->AsObject() : nullptr;
            if (!Item) { continue; }
            TSharedPtr<FAgentEvent> Event = MakeShared<FAgentEvent>();
            int32 Type = 0;
            if (!Item->TryGetNumberField(TEXT("sequence"), Event->Sequence)
                || !Item->TryGetNumberField(TEXT("type"), Type)
                || Type < 0 || Type > static_cast<int32>(EAgentEventType::RunFailed)) { continue; }
            ReadGuid(*Item, TEXT("run_id"), Event->RunId);
            ReadDate(*Item, TEXT("time"), Event->Time);
            Item->TryGetStringField(TEXT("summary"), Event->Summary);
            Item->TryGetStringField(TEXT("detail"), Event->Detail);
            Event->Type = static_cast<EAgentEventType>(Type);
            Session.Events.Add(Event);
            Session.NextEventSequence = FMath::Max(Session.NextEventSequence, Event->Sequence + 1);
        }
    }
    if (Json.TryGetArrayField(TEXT("run_history"), Array))
    {
        for (const TSharedPtr<FJsonValue>& Entry : *Array)
        {
            const TSharedPtr<FJsonObject> Item = Entry ? Entry->AsObject() : nullptr;
            FAgentRunInputSnapshot Snapshot;
            // Forks retain the source Run IDs and source SessionId as historical provenance.
            if (Item && ReadSnapshot(*Item, Snapshot))
            { Session.RunHistory.Add(MoveTemp(Snapshot)); }
        }
    }
    if (!Session.RunHistory.IsEmpty())
    {
        Session.Runner.LastSnapshot = MakeShared<FAgentRunInputSnapshot>(Session.RunHistory.Last());
        Session.Runner.CurrentRunId = Session.RunHistory.Last().RunId;
    }
    return true;
}
}

FString FAgentWorkbenchHistory::DefaultDirectory()
{
    return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AgentWorkbench"), TEXT("Sessions"));
}

bool FAgentWorkbenchHistory::Save(const FAgentSession& Session, const FString& Directory)
{
    check(IsInGameThread());
    if (!Session.SessionId.IsValid() || !IFileManager::Get().MakeDirectory(*Directory, true)) { return false; }
    FString Json;
    if (!FJsonSerializer::Serialize(SessionToJson(Session), TJsonWriterFactory<>::Create(&Json))) { return false; }
    const FString Dest = FPaths::Combine(Directory, Session.SessionId.ToString(EGuidFormats::Digits) + TEXT(".json"));
    const FString Temp = Dest + TEXT(".tmp");
    return FFileHelper::SaveStringToFile(Json, *Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
        && IFileManager::Get().Move(*Dest, *Temp, true);
}

void FAgentWorkbenchHistory::LoadAll(const FString& Directory, TArray<TSharedPtr<FAgentSession>>& OutSessions,
    TArray<FString>& OutErrors, bool bValidateAssets)
{
    check(IsInGameThread());
    TArray<FString> Files;
    IFileManager::Get().FindFiles(Files, *FPaths::Combine(Directory, TEXT("*.json")), true, false);
    for (const FString& File : Files)
    {
        const FString Path = FPaths::Combine(Directory, File);
        FString Text;
        TSharedPtr<FJsonObject> Json;
        if (!FFileHelper::LoadFileToString(Text, *Path)
            || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) || !Json)
        { OutErrors.Add(File + TEXT(": 无法解析")); continue; }
        TSharedPtr<FAgentSession> Session = MakeShared<FAgentSession>();
        if (!SessionFromJson(*Json, *Session)
            || File != Session->SessionId.ToString(EGuidFormats::Digits) + TEXT(".json"))
        { OutErrors.Add(File + TEXT(": Session 格式或文件名无效")); continue; }
        if (bValidateAssets)
        {
            for (const TSharedPtr<FAgentCandidate>& Candidate : Session->Candidates)
            {
                FString Normalized;
                Candidate->ValidationStatus = FAgentAssetContextService::NormalizeObjectPath(Candidate->ObjectPath, Normalized)
                    ? FAgentAssetContextService::LookupAsset(FSoftObjectPath(Normalized)).Status
                    : EAgentCandidateValidation::Missing;
            }
        }
        OutSessions.Add(Session);
    }
}
