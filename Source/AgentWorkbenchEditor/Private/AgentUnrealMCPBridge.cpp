#include "AgentUnrealMCPBridge.h"
#include "AgentToolSchemaValidator.h"
#include "AgentToolBridge.h"
#include "AgentWorkbenchSession.h"

#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "IModelContextProtocolModule.h"
#include "IModelContextProtocolTool.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "Misc/AutomationTest.h"
#include "HAL/PlatformTime.h"
#include "ModelContextProtocolSession.h"
#include "Serialization/JsonSerializer.h"
#include "ToolsetRegistry/Toolset.h"
#include "ToolsetRegistry/ToolsetRegistry.h"
#include "ToolsetRegistry/ToolsetRegistrySubsystem.h"

namespace
{
constexpr int32 MaxToolResultChars = 1024 * 1024;
constexpr TCHAR UnrealPrefix[] = TEXT("ue_mcp.");

enum class EToolOrigin : uint8 { Toolset, TopLevel };

struct FUnrealBinding
{
    EToolOrigin Origin = EToolOrigin::Toolset;
    FString FullName;
    FString Fingerprint;
    FGuid InstanceId;
    TWeakPtr<UE::ToolsetRegistry::FToolset> Toolset;
    TWeakPtr<IModelContextProtocolTool> TopLevel;
    TSharedPtr<FJsonObject> InputSchema;
    bool bRequiresApproval = true;

    FString HandlerId() const
    {
        return FString::Printf(TEXT("ue_mcp.%s.%s.%s"),
            Origin == EToolOrigin::Toolset ? TEXT("toolset") : TEXT("top"),
            *InstanceId.ToString(EGuidFormats::Digits), *Fingerprint);
    }
};

// Accessed only on the game thread. Weak references make removal/reload visible
// and a new instance receives a new ID even if it reuses a tool name.
TMap<FString, FUnrealBinding> CurrentBindings;

FString SerializeJson(const TSharedRef<FJsonObject>& Object)
{
    FString Text;
    FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Text));
    return Text;
}

bool ParseObject(const FString& Text, TSharedPtr<FJsonObject>& OutObject)
{
    return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), OutObject) && OutObject.IsValid();
}

bool IsObjectSchema(const TSharedPtr<FJsonObject>& Schema)
{
    FString Type;
    return Schema.IsValid() && Schema->TryGetStringField(TEXT("type"), Type) && Type == TEXT("object");
}

FString RegistryNameFor(const FString& FullName, EToolOrigin Origin)
{
    FString Readable;
    Readable.Reserve(70);
    for (const TCHAR Character : FullName)
    {
        if (Readable.Len() >= 70) { break; }
        const bool bAllowed = (Character >= TEXT('a') && Character <= TEXT('z'))
            || (Character >= TEXT('A') && Character <= TEXT('Z'))
            || (Character >= TEXT('0') && Character <= TEXT('9'))
            || Character == TEXT('_') || Character == TEXT('-') || Character == TEXT('.');
        Readable.AppendChar(bAllowed ? Character : TEXT('_'));
    }
    const FString Identity = (Origin == EToolOrigin::Toolset ? TEXT("toolset:") : TEXT("top:")) + FullName;
    return FString(UnrealPrefix) + Readable + TEXT("_") + FMD5::HashAnsiString(*Identity);
}

bool IsToolSearchMetaTool(const FString& Name)
{
    return Name.Equals(TEXT("call_tool"), ESearchCase::IgnoreCase)
        || Name.Equals(TEXT("list_toolsets"), ESearchCase::IgnoreCase)
        || Name.Equals(TEXT("describe_toolset"), ESearchCase::IgnoreCase);
}

bool SameInstance(const FUnrealBinding& Previous, const FUnrealBinding& Current)
{
    if (Previous.Origin != Current.Origin || Previous.FullName != Current.FullName
        || Previous.Fingerprint != Current.Fingerprint)
    {
        return false;
    }
    if (Current.Origin == EToolOrigin::Toolset)
    {
        return Current.Toolset.IsValid() && Previous.Toolset.Pin() == Current.Toolset.Pin();
    }
    return Current.TopLevel.IsValid() && Previous.TopLevel.Pin() == Current.TopLevel.Pin();
}

void AddBinding(TMap<FString, FUnrealBinding>& Next,
    const TMap<FString, FUnrealBinding>& Previous, TArray<FMCPToolDefinition>& Definitions,
    FUnrealBinding Binding, const FString& Description, const TSharedPtr<FJsonObject>& Schema)
{
    if (!IsObjectSchema(Schema)) { return; }
    const FString RegistryName = RegistryNameFor(Binding.FullName, Binding.Origin);
    if (Next.Contains(RegistryName)) { return; }
    if (const FUnrealBinding* Old = Previous.Find(RegistryName); Old && SameInstance(*Old, Binding))
    {
        Binding.InstanceId = Old->InstanceId;
    }
    else
    {
        Binding.InstanceId = FGuid::NewGuid();
    }
    FMCPToolDefinition Definition;
    Definition.Name = RegistryName;
    Definition.Description = FString::Printf(TEXT("UE MCP tool %s. %s"), *Binding.FullName, *Description);
    if (Binding.FullName == TEXT("editor_toolset.toolsets.blueprint.BlueprintTools.write_graph_dsl"))
    {
        Definition.Description += TEXT(" This tool compiles the Blueprint itself. Do not call compile_blueprint again unless later edits require it.");
    }
    if (Binding.bRequiresApproval)
    {
        Definition.Description += TEXT(" Approval follows the run's selected permission mode.");
    }
    Definition.InputSchema = Schema;
    Binding.InputSchema = Schema;
    Definition.NativeHandlerId = Binding.HandlerId();
    Next.Add(RegistryName, MoveTemp(Binding));
    Definitions.Add(MoveTemp(Definition));
}

struct FPendingCall
{
    bool bFinished = false; // Game thread only.
    FTSTicker::FDelegateHandle TimeoutHandle;
    TFunction<void(FMCPToolResult)> Completion;
    TSharedPtr<IModelContextProtocolTool> TopLevel;
    FModelContextProtocolToolRequestId RequestId;
};

void Finish(const TSharedRef<FPendingCall>& Pending, FMCPToolResult Result, bool bFromTimeout = false)
{
    check(IsInGameThread());
    if (Pending->bFinished) { return; }
    Pending->bFinished = true;
    if (Pending->TimeoutHandle.IsValid() && !bFromTimeout)
    {
        FTSTicker::GetCoreTicker().RemoveTicker(Pending->TimeoutHandle);
    }
    Pending->TimeoutHandle.Reset();
    if (bFromTimeout && Pending->TopLevel.IsValid())
    {
        Pending->TopLevel->CancelAsync(Pending->RequestId);
    }
    Pending->TopLevel.Reset();
    TFunction<void(FMCPToolResult)> Completion = MoveTemp(Pending->Completion);
    Completion(MoveTemp(Result));
}

FMCPToolResult ConvertTopLevelResult(const FModelContextProtocolToolResult& Result)
{
    if (!Result.JsonObject.IsValid())
    {
        return FMCPToolResult::Failure(TEXT("UE MCP tool returned no result object."));
    }
    const FString Raw = SerializeJson(Result.JsonObject.ToSharedRef());
    if (Raw.Len() > MaxToolResultChars)
    {
        return FMCPToolResult::Failure(TEXT("UE MCP tool result exceeded the 1 MiB display limit."));
    }
    FMCPToolResult Converted;
    Result.JsonObject->TryGetBoolField(TEXT("isError"), Converted.bIsError);
    const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
    if (Result.JsonObject->TryGetArrayField(TEXT("content"), Content))
    {
        Converted.Content = *Content;
    }
    const TSharedPtr<FJsonObject>* Structured = nullptr;
    if (Result.JsonObject->TryGetObjectField(TEXT("structuredContent"), Structured))
    {
        Converted.StructuredContent = *Structured;
    }
    if (Converted.Content.IsEmpty() && !Converted.StructuredContent.IsValid())
    {
        Converted = FMCPToolResult::Success(Raw);
        Result.JsonObject->TryGetBoolField(TEXT("isError"), Converted.bIsError);
    }
    return Converted;
}
}

TArray<FMCPToolDefinition> FAgentUnrealMCPBridge::ListAvailable()
{
    check(IsInGameThread());
    const TMap<FString, FUnrealBinding> Previous = MoveTemp(CurrentBindings);
    TMap<FString, FUnrealBinding> Next;
    TArray<FMCPToolDefinition> Definitions;
    TSet<FString> RegisteredToolsetNames;
    if (UToolsetRegistrySubsystem* Subsystem = GEditor
        ? GEditor->GetEditorSubsystem<UToolsetRegistrySubsystem>() : nullptr)
    {
        UE::ToolsetRegistry::FToolsetRegistry& Registry = Subsystem->ToolsetRegistry;
        Registry.ForEachToolset([&](const FString& ToolsetName, const UE::ToolsetRegistry::FToolset& Toolset)
        {
            const TSharedPtr<UE::ToolsetRegistry::FToolset> ToolsetPtr = Registry.Find(ToolsetName);
            TSharedPtr<FJsonObject> Catalog;
            if (!ToolsetPtr.IsValid() || !ParseObject(Toolset.GetJsonSchema(), Catalog)) { return; }
            const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
            if (!Catalog->TryGetArrayField(TEXT("tools"), Tools)) { return; }
            for (const TSharedPtr<FJsonValue>& Value : *Tools)
            {
                const TSharedPtr<FJsonObject> Entry = Value.IsValid() && Value->Type == EJson::Object
                    ? Value->AsObject() : nullptr;
                if (!Entry.IsValid()) { continue; }
                FString FullName;
                FString Description;
                if (!Entry->TryGetStringField(TEXT("name"), FullName)) { continue; }
                Entry->TryGetStringField(TEXT("description"), Description);
                const auto Parsed = UE::ToolsetRegistry::FToolDescriptor::FromString(FullName);
                if (!Parsed.HasValue() || Parsed.GetValue().ToolsetName != ToolsetName
                    || !Toolset.IsToolEnabled(FullName))
                {
                    continue;
                }
                const TSharedPtr<FJsonObject>* Schema = nullptr;
                if (!Entry->TryGetObjectField(TEXT("inputSchema"), Schema)) { continue; }
                FUnrealBinding Binding;
                Binding.Origin = EToolOrigin::Toolset;
                Binding.FullName = FullName;
                Binding.Fingerprint = FMD5::HashAnsiString(*SerializeJson(Entry.ToSharedRef()));
                Binding.Toolset = ToolsetPtr;
                Binding.bRequiresApproval = true;
                AddBinding(Next, Previous, Definitions, MoveTemp(Binding), Description, *Schema);
                RegisteredToolsetNames.Add(FullName);
            }
        });
    }
    if (IModelContextProtocolModule* Module = IModelContextProtocolModule::Get())
    {
        for (const TSharedRef<IModelContextProtocolTool>& Tool : Module->GetTools())
        {
            const FString FullName = Tool->GetName();
            // These meta-tools dispatch arbitrary targets and bypass per-target checks.
            if (IsToolSearchMetaTool(FullName) || RegisteredToolsetNames.Contains(FullName)) { continue; }
            TSharedPtr<FJsonObject> Schema = Tool->GetInputJsonSchema();
            if (!Schema.IsValid())
            {
                Schema = MakeShared<FJsonObject>();
                Schema->SetStringField(TEXT("type"), TEXT("object"));
                Schema->SetBoolField(TEXT("additionalProperties"), false);
            }
            FUnrealBinding Binding;
            Binding.Origin = EToolOrigin::TopLevel;
            Binding.FullName = FullName;
            Binding.Fingerprint = FMD5::HashAnsiString(*(FullName + Tool->GetDescription() + SerializeJson(Schema.ToSharedRef())));
            Binding.TopLevel = Tool;
            Binding.bRequiresApproval = true;
            AddBinding(Next, Previous, Definitions, MoveTemp(Binding), Tool->GetDescription(), Schema);
        }
    }
    CurrentBindings = MoveTemp(Next);
    Definitions.Sort([](const FMCPToolDefinition& A, const FMCPToolDefinition& B)
    {
        return A.Name < B.Name;
    });
    return Definitions;
}

FMCPToolResult FAgentUnrealMCPBridge::SearchCatalog(const TMap<FString, FString>& FrozenHandlerIds,
    const FString& Query, int32 Offset, int32 Limit)
{
    check(IsInGameThread());
    if (Offset < 0 || Limit < 1 || Limit > 100)
    {
        return FMCPToolResult::ProtocolError(-32602, TEXT("offset must be non-negative and limit must be 1..100."));
    }
    const TArray<FMCPToolDefinition> Definitions = ListAvailable();
    TArray<TSharedPtr<FJsonValue>> Matches;
    for (const FMCPToolDefinition& Definition : Definitions)
    {
        const FUnrealBinding* Binding = CurrentBindings.Find(Definition.Name);
        const FString* FrozenId = FrozenHandlerIds.Find(Definition.Name);
        if (!Binding || !FrozenId || *FrozenId != Binding->HandlerId()
            || (!Query.IsEmpty() && !Binding->FullName.Contains(Query, ESearchCase::IgnoreCase)
            && !Definition.Description.Contains(Query, ESearchCase::IgnoreCase)))
        {
            continue;
        }
        const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
        Item->SetStringField(TEXT("registry_name"), Definition.Name);
        Item->SetStringField(TEXT("unreal_name"), Binding->FullName);
        Item->SetStringField(TEXT("description"), Definition.Description);
        Item->SetBoolField(TEXT("requires_approval"), true);
        Matches.Add(MakeShared<FJsonValueObject>(Item));
    }
    const int32 End = static_cast<int32>(FMath::Min<int64>(Matches.Num(), static_cast<int64>(Offset) + Limit));
    TArray<TSharedPtr<FJsonValue>> Page;
    for (int32 Index = FMath::Min(Offset, Matches.Num()); Index < End; ++Index)
    {
        Page.Add(Matches[Index]);
    }
    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("items"), MoveTemp(Page));
    Data->SetNumberField(TEXT("total"), Matches.Num());
    Data->SetNumberField(TEXT("offset"), Offset);
    Data->SetBoolField(TEXT("has_more"), End < Matches.Num());
    return FMCPToolResult::Success(SerializeJson(Data), Data);
}

FMCPToolResult FAgentUnrealMCPBridge::DescribeCatalogTool(
    const TMap<FString, FString>& FrozenHandlerIds, const FString& RegistryName)
{
    check(IsInGameThread());
    const TArray<FMCPToolDefinition> Definitions = ListAvailable();
    const FMCPToolDefinition* Definition = Definitions.FindByPredicate([&](const FMCPToolDefinition& Item)
    {
        return Item.Name == RegistryName;
    });
    const FUnrealBinding* Binding = CurrentBindings.Find(RegistryName);
    const FString* FrozenId = FrozenHandlerIds.Find(RegistryName);
    if (!Definition || !Binding || !FrozenId || *FrozenId != Binding->HandlerId())
    {
        return FMCPToolResult::ProtocolError(-32602, TEXT("UE MCP tool is not currently available."));
    }
    const TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("registry_name"), RegistryName);
    Data->SetStringField(TEXT("unreal_name"), Binding->FullName);
    Data->SetStringField(TEXT("description"), Definition->Description);
    Data->SetBoolField(TEXT("requires_approval"), true);
    Data->SetObjectField(TEXT("input_schema"), Definition->InputSchema);
    return FMCPToolResult::Success(SerializeJson(Data), Data);
}

bool FAgentUnrealMCPBridge::IsCurrentBinding(const FString& RegistryName, const FString& FrozenHandlerId)
{
    check(IsInGameThread());
    ListAvailable();
    const FUnrealBinding* Binding = CurrentBindings.Find(RegistryName);
    return Binding && Binding->HandlerId() == FrozenHandlerId;
}

bool FAgentUnrealMCPBridge::ValidateArguments(const FString& RegistryName,
    const FString& FrozenHandlerId, const TSharedRef<FJsonObject>& Arguments, FString& OutError)
{
    check(IsInGameThread());
    ListAvailable();
    const FUnrealBinding* Binding = CurrentBindings.Find(RegistryName);
    if (!Binding || Binding->HandlerId() != FrozenHandlerId || !Binding->InputSchema.IsValid())
    {
        OutError = TEXT("UE MCP tool was removed or changed since this run started.");
        return false;
    }
    return FAgentToolSchemaValidator::Validate(Binding->InputSchema, Arguments, OutError);
}

FString FAgentUnrealMCPBridge::DisplayName(const FString& RegistryName, const FString& FrozenHandlerId)
{
    check(IsInGameThread());
    ListAvailable();
    const FUnrealBinding* Binding = CurrentBindings.Find(RegistryName);
    return Binding && Binding->HandlerId() == FrozenHandlerId ? Binding->FullName : RegistryName;
}

bool FAgentUnrealMCPBridge::RequiresApproval(const FString& RegistryName, const FString& FrozenHandlerId)
{
    check(IsInGameThread());
    ListAvailable();
    const FUnrealBinding* Binding = CurrentBindings.Find(RegistryName);
    return !Binding || Binding->HandlerId() != FrozenHandlerId || Binding->bRequiresApproval;
}

void FAgentUnrealMCPBridge::Execute(const FString& RegistryName, const FString& FrozenHandlerId,
    const TSharedRef<FJsonObject>& Arguments, double TimeoutSeconds, bool bApproved,
    TFunction<void(FMCPToolResult)> Completion)
{
    check(IsInGameThread());
    check(Completion);
    ListAvailable();
    const FUnrealBinding* Binding = CurrentBindings.Find(RegistryName);
    if (!Binding || Binding->HandlerId() != FrozenHandlerId)
    {
        Completion(FMCPToolResult::ProtocolError(-32602, TEXT("UE MCP tool was removed or changed since this run started.")));
        return;
    }
    FString SchemaError;
    if (!FAgentToolSchemaValidator::Validate(Binding->InputSchema, Arguments, SchemaError))
    {
        Completion(FMCPToolResult::ProtocolError(-32602, SchemaError));
        return;
    }
    if (Binding->bRequiresApproval && !bApproved)
    {
        Completion(FMCPToolResult::ProtocolError(-32602, TEXT("UE MCP tool requires explicit user approval before execution.")));
        return;
    }
    const TSharedRef<FPendingCall> Pending = MakeShared<FPendingCall>();
    Pending->Completion = MoveTemp(Completion);
    const float DelaySeconds = static_cast<float>(FMath::Max(0.01, TimeoutSeconds));
    Pending->TimeoutHandle = FTSTicker::GetCoreTicker().AddTicker(TEXT("AgentUnrealMCPToolTimeout"), DelaySeconds,
        [Pending](float)
        {
            Finish(Pending, FMCPToolResult::Failure(TEXT("UE MCP tool timed out.")), true);
            return false;
        });
    if (Binding->Origin == EToolOrigin::Toolset)
    {
        UToolsetRegistrySubsystem* Subsystem = GEditor
            ? GEditor->GetEditorSubsystem<UToolsetRegistrySubsystem>() : nullptr;
        const auto Descriptor = UE::ToolsetRegistry::FToolDescriptor::FromString(Binding->FullName);
        const TSharedPtr<UE::ToolsetRegistry::FToolset> CurrentToolset = Subsystem && Descriptor.HasValue()
            ? Subsystem->ToolsetRegistry.Find(Descriptor.GetValue().ToolsetName) : nullptr;
        if (!CurrentToolset.IsValid() || CurrentToolset != Binding->Toolset.Pin())
        {
            Finish(Pending, FMCPToolResult::ProtocolError(-32602, TEXT("UE toolset changed before dispatch.")));
            return;
        }
        Subsystem->ToolsetRegistry.ExecuteTool(Descriptor.GetValue(), SerializeJson(Arguments)).Then(
            [Pending](TFuture<TValueOrError<FString, FString>> Future) mutable
            {
                TValueOrError<FString, FString> Value = Future.Get();
                FMCPToolResult Result = Value.HasError()
                    ? FMCPToolResult::Failure(Value.GetError())
                    : (Value.GetValue().Len() > MaxToolResultChars
                        ? FMCPToolResult::Failure(TEXT("UE toolset result exceeded the 1 MiB display limit."))
                        : FMCPToolResult::Success(Value.GetValue()));
                if (IsInGameThread()) { Finish(Pending, MoveTemp(Result)); }
                else
                {
                    AsyncTask(ENamedThreads::GameThread, [Pending, Result = MoveTemp(Result)]() mutable
                    {
                        Finish(Pending, MoveTemp(Result));
                    });
                }
            });
        return;
    }
    Pending->TopLevel = Binding->TopLevel.Pin();
    if (!Pending->TopLevel.IsValid())
    {
        Finish(Pending, FMCPToolResult::ProtocolError(-32602, TEXT("UE MCP tool disappeared before dispatch.")));
        return;
    }
    Pending->RequestId = FModelContextProtocolToolRequestId(
        MakeShared<FJsonValueString>(FGuid::NewGuid().ToString(EGuidFormats::Digits)));
    const TSharedPtr<IModelContextProtocolTool> Tool = Pending->TopLevel;
    Tool->RunAsync(Pending->RequestId, Arguments,
        [Pending](const FModelContextProtocolToolResult& Value)
        {
            FMCPToolResult Result = ConvertTopLevelResult(Value);
            if (IsInGameThread()) { Finish(Pending, MoveTemp(Result)); }
            else
            {
                AsyncTask(ENamedThreads::GameThread, [Pending, Result = MoveTemp(Result)]() mutable
                {
                    Finish(Pending, MoveTemp(Result));
                });
            }
        });
}

bool FAgentUnrealMCPBridge::IsUnrealToolName(const FString& RegistryName)
{
    return RegistryName.StartsWith(UnrealPrefix, ESearchCase::CaseSensitive);
}

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentUnrealMCPBridgeCatalogTest,
    "AgentWorkbench.ToolsBridge.UnrealMCPCatalog",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentUnrealMCPBridgeCatalogTest::RunTest(const FString& Parameters)
{
    const TArray<FMCPToolDefinition> First = FAgentUnrealMCPBridge::ListAvailable();
    AddInfo(FString::Printf(TEXT("UE MCP catalog contains %d callable tools."), First.Num()));
    int32 InvalidSchemas = 0;
    TArray<FString> InvalidSchemaDetails;
    for (const FMCPToolDefinition& Definition : First)
    {
        FString Error;
        if (!FAgentToolSchemaValidator::Validate(Definition.InputSchema,
            MakeShared<FJsonObject>(), Error) && Error.StartsWith(TEXT("$schema")))
        {
            ++InvalidSchemas;
            InvalidSchemaDetails.Add(Definition.Description + TEXT(": ") + Error);
        }
    }
    TestEqual(FString::Printf(TEXT("All discovered tool schemas can be validated (issues: %s)"),
        *FString::Join(InvalidSchemaDetails, TEXT(" | "))), InvalidSchemas, 0);
    if (FModuleManager::Get().IsModuleLoaded(FName(TEXT("EditorToolset"))))
    {
        const FMCPToolDefinition* Variables = First.FindByPredicate([](const FMCPToolDefinition& Definition)
        {
            return Definition.Description.Contains(TEXT("BlueprintTools.list_variables"));
        });
        TestTrue(TEXT("Enabled EditorToolset exposes BlueprintTools.list_variables"), Variables != nullptr);
    }
    const TArray<FMCPToolDefinition> Second = FAgentUnrealMCPBridge::ListAvailable();
    TestEqual(TEXT("Refresh retains the same tool count"), Second.Num(), First.Num());
    for (const FMCPToolDefinition& Definition : First)
    {
        const FMCPToolDefinition* Refreshed = Second.FindByPredicate([&](const FMCPToolDefinition& Other)
        {
            return Other.Name == Definition.Name;
        });
        TestTrue(TEXT("Refresh retains each frozen tool identity"), Refreshed
            && Refreshed->NativeHandlerId == Definition.NativeHandlerId);
    }

    FAgentRunInputSnapshot Snapshot;
    FAgentToolBridge::FreezeAvailableTools(Snapshot);
    Snapshot.bToolListFrozen = true;
    const FMCPToolResult Catalog = FAgentUnrealMCPBridge::SearchCatalog(
        Snapshot.AllowedToolHandlerIds, FString(), 0, 100);
    TestFalse(TEXT("Catalog search succeeds"), Catalog.bIsError);
    TestTrue(TEXT("Catalog search has structured results"), Catalog.StructuredContent.IsValid());
    if (First.IsEmpty())
    {
        AddWarning(TEXT("No enabled UE MCP tools are loaded; catalog routing could not be exercised."));
        return true;
    }
    const FMCPToolDefinition& Target = First[0];
    const FMCPToolResult Description = FAgentUnrealMCPBridge::DescribeCatalogTool(
        Snapshot.AllowedToolHandlerIds, Target.Name);
    TestFalse(TEXT("Catalog describes a frozen tool"), Description.bIsError);
    TestTrue(TEXT("Description has the target schema"), Description.StructuredContent.IsValid()
        && Description.StructuredContent->HasField(TEXT("input_schema")));

    // A known no-argument tool exercises search -> describe -> controlled call
    // without invoking any editor operation in automation.
    const FMCPToolDefinition* NoArgumentTool = First.FindByPredicate([](const FMCPToolDefinition& Definition)
    {
        return Definition.Description.Contains(TEXT("AgentSkillToolset.ListSkills"));
    });
    if (!NoArgumentTool)
    {
        AddWarning(TEXT("AgentSkillToolset.ListSkills is not loaded; controlled call path could not be exercised."));
        return true;
    }
    const FMCPToolResult Found = FAgentUnrealMCPBridge::SearchCatalog(
        Snapshot.AllowedToolHandlerIds, TEXT("AgentSkillToolset.ListSkills"), 0, 10);
    const TArray<TSharedPtr<FJsonValue>>* FoundItems = nullptr;
    FString DiscoveredName;
    if (Found.StructuredContent.IsValid()
        && Found.StructuredContent->TryGetArrayField(TEXT("items"), FoundItems)
        && FoundItems && FoundItems->Num() == 1 && (*FoundItems)[0].IsValid()
        && (*FoundItems)[0]->Type == EJson::Object)
    {
        (*FoundItems)[0]->AsObject()->TryGetStringField(TEXT("registry_name"), DiscoveredName);
    }
    TestEqual(TEXT("Catalog search returns the callable registry name"), DiscoveredName, NoArgumentTool->Name);
    if (DiscoveredName.IsEmpty()) { return true; }
    const FMCPToolResult Discovered = FAgentUnrealMCPBridge::DescribeCatalogTool(
        Snapshot.AllowedToolHandlerIds, DiscoveredName);
    TestTrue(TEXT("Discovered tool description includes its input schema"),
        !Discovered.bIsError && Discovered.StructuredContent.IsValid()
        && Discovered.StructuredContent->HasField(TEXT("input_schema")));
    const TSharedRef<FJsonObject> Call = MakeShared<FJsonObject>();
    Call->SetStringField(TEXT("registry_name"), DiscoveredName);
    Call->SetObjectField(TEXT("arguments"), MakeShared<FJsonObject>());
    FString ResolvedName;
    TSharedPtr<FJsonObject> ResolvedArguments;
    FString Error;
    TestTrue(TEXT("Controlled call resolves a frozen UE target"), FAgentToolBridge::ResolveApprovalTarget(
        Snapshot, TEXT("ue_mcp.call_tool"), Call, ResolvedName, ResolvedArguments, Error));
    TestEqual(TEXT("Controlled call keeps its actual target"), ResolvedName, NoArgumentTool->Name);
    TestTrue(TEXT("Resolved target arguments are an object"), ResolvedArguments.IsValid());
    const TSharedRef<FJsonObject> ExtraArguments = MakeShared<FJsonObject>();
    ExtraArguments->SetStringField(TEXT("unexpected"), TEXT("ignored by the engine"));
    Call->SetObjectField(TEXT("arguments"), ExtraArguments);
    TestFalse(TEXT("Unknown argument is rejected before approval"),
        FAgentToolBridge::ResolveApprovalTarget(Snapshot, TEXT("ue_mcp.call_tool"), Call,
            ResolvedName, ResolvedArguments, Error));
    Call->SetObjectField(TEXT("arguments"), MakeShared<FJsonObject>());

    const FMCPToolDefinition* ArrayTool = First.FindByPredicate([](const FMCPToolDefinition& Definition)
    {
        return Definition.Description.Contains(TEXT("AgentSkillToolset.GetSkills"));
    });
    if (ArrayTool)
    {
        const TArray<TSharedPtr<FJsonValue>>* Required = nullptr;
        TestTrue(TEXT("GetSkills declares a required argument"),
            ArrayTool->InputSchema->TryGetArrayField(TEXT("required"), Required)
            && Required && Required->Num() == 1);
        FString ArrayError;
        TestFalse(TEXT("Missing required UE argument is rejected"),
            FAgentUnrealMCPBridge::ValidateArguments(ArrayTool->Name,
                ArrayTool->NativeHandlerId, MakeShared<FJsonObject>(), ArrayError));
        if (Required && Required->Num() == 1 && (*Required)[0].IsValid())
        {
            const TSharedRef<FJsonObject> InvalidArray = MakeShared<FJsonObject>();
            TArray<TSharedPtr<FJsonValue>> Items;
            Items.Add(MakeShared<FJsonValueNumber>(7));
            InvalidArray->SetArrayField((*Required)[0]->AsString(), MoveTemp(Items));
            TestFalse(TEXT("Invalid nested array item is rejected"),
                FAgentUnrealMCPBridge::ValidateArguments(ArrayTool->Name,
                    ArrayTool->NativeHandlerId, InvalidArray, ArrayError));
        }
    }
    bool bCompleted = false;
    FMCPToolResult Result;
    FAgentToolBridge::Execute(Snapshot, TEXT("ue_mcp.call_tool"), Call, 1.0, false,
        [&bCompleted, &Result](FMCPToolResult InResult)
        {
            bCompleted = true;
            Result = MoveTemp(InResult);
        });
    TestTrue(TEXT("Unapproved controlled call completes synchronously"), bCompleted);
    TestTrue(TEXT("Unapproved controlled call cannot dispatch"), Result.bIsError);

    Snapshot.AllowedToolHandlerIds[NoArgumentTool->Name] += TEXT("changed");
    TestFalse(TEXT("Changed target identity is rejected before approval"),
        FAgentToolBridge::ResolveApprovalTarget(Snapshot, TEXT("ue_mcp.call_tool"), Call,
            ResolvedName, ResolvedArguments, Error));
    Snapshot.AllowedToolHandlerIds[NoArgumentTool->Name] = NoArgumentTool->NativeHandlerId;

    struct FActualCall
    {
        bool bDone = false;
        FMCPToolResult Result;
    };
    const TSharedRef<FActualCall> Actual = MakeShared<FActualCall>();
    FAgentToolBridge::Execute(Snapshot, TEXT("ue_mcp.call_tool"), Call, 10.0, true,
        [Actual](FMCPToolResult InResult)
        {
            Actual->bDone = true;
            Actual->Result = MoveTemp(InResult);
        });
    const double Deadline = FPlatformTime::Seconds() + 12.0;
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Actual, Deadline]()
    {
        if (!Actual->bDone && FPlatformTime::Seconds() < Deadline) { return false; }
        TestTrue(TEXT("Approved read-only UE MCP tool completed"), Actual->bDone);
        if (!Actual->bDone) { return true; }
        TestFalse(TEXT("ListSkills returned without a tool error"), Actual->Result.bIsError);
        if (Actual->Result.bIsError) { return true; }
        TestTrue(TEXT("ListSkills returned a text content block"),
            Actual->Result.Content.Num() == 1 && Actual->Result.Content[0].IsValid()
            && Actual->Result.Content[0]->Type == EJson::Object);
        if (Actual->Result.Content.Num() == 1 && Actual->Result.Content[0].IsValid()
            && Actual->Result.Content[0]->Type == EJson::Object)
        {
            FString ResponseText;
            const TSharedPtr<FJsonObject> Block = Actual->Result.Content[0]->AsObject();
            TSharedPtr<FJsonObject> ResponseObject;
            TestTrue(TEXT("ListSkills result contains parseable JSON"),
                Block.IsValid() && Block->TryGetStringField(TEXT("text"), ResponseText)
                && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ResponseText), ResponseObject)
                && ResponseObject.IsValid() && ResponseObject->HasField(TEXT("returnValue")));
        }
        return true;
    }));
    return true;
}
#endif
