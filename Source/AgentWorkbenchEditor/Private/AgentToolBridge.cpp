#include "AgentToolBridge.h"

#include "AgentAssetContextService.h"
#include "AgentUnrealMCPBridge.h"
#include "AgentWorkbenchSession.h"
#include "Async/Async.h"
#include "BSHarnessToolsModule.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "MCPToolRegistry.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/SecureHash.h"

namespace
{
constexpr TCHAR CatalogSearchName[] = TEXT("ue_mcp.catalog.search");
constexpr TCHAR CatalogDescribeName[] = TEXT("ue_mcp.catalog.describe");
constexpr TCHAR CatalogCallName[] = TEXT("ue_mcp.call_tool");
constexpr TCHAR CatalogSearchId[] = TEXT("ue_mcp.meta.search.v1");
constexpr TCHAR CatalogDescribeId[] = TEXT("ue_mcp.meta.describe.v1");
constexpr TCHAR CatalogCallId[] = TEXT("ue_mcp.meta.call.v1");

FMCPToolDefinition MakeMetaDefinition(const TCHAR* Name, const TCHAR* HandlerId,
    const TCHAR* Description, const TArray<FString>& StringProperties,
    const TArray<FString>& Required)
{
    FMCPToolDefinition Definition;
    Definition.Name = Name;
    Definition.NativeHandlerId = HandlerId;
    Definition.Description = Description;
    Definition.InputSchema = MakeShared<FJsonObject>();
    Definition.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
    const TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
    for (const FString& Property : StringProperties)
    {
        const TSharedRef<FJsonObject> Rule = MakeShared<FJsonObject>();
        Rule->SetStringField(TEXT("type"), TEXT("string"));
        Properties->SetObjectField(Property, Rule);
    }
    if (Definition.Name == CatalogSearchName)
    {
        for (const TCHAR* Property : {TEXT("offset"), TEXT("limit")})
        {
            const TSharedRef<FJsonObject> Rule = MakeShared<FJsonObject>();
            Rule->SetStringField(TEXT("type"), TEXT("integer"));
            const bool bOffset = FString(Property) == TEXT("offset");
            Rule->SetNumberField(TEXT("minimum"), bOffset ? 0 : 1);
            Rule->SetNumberField(TEXT("default"), bOffset ? 0 : 50);
            Properties->SetObjectField(Property, Rule);
        }
    }
    if (Definition.Name == CatalogCallName)
    {
        const TSharedRef<FJsonObject> Rule = MakeShared<FJsonObject>();
        Rule->SetStringField(TEXT("type"), TEXT("object"));
        Properties->SetObjectField(TEXT("arguments"), Rule);
    }
    Definition.InputSchema->SetObjectField(TEXT("properties"), Properties);
    TArray<TSharedPtr<FJsonValue>> RequiredJson;
    for (const FString& Property : Required)
    {
        RequiredJson.Add(MakeShared<FJsonValueString>(Property));
    }
    Definition.InputSchema->SetArrayField(TEXT("required"), MoveTemp(RequiredJson));
    return Definition;
}

TArray<FMCPToolDefinition> MetaDefinitions()
{
    TArray<FMCPToolDefinition> Definitions;
    Definitions.Add(MakeMetaDefinition(CatalogSearchName, CatalogSearchId,
        TEXT("Search UE MCP tools available when this run started. Returns names and summaries; no engine tool is executed."),
        {TEXT("query")}, {}));
    Definitions.Add(MakeMetaDefinition(CatalogDescribeName, CatalogDescribeId,
        TEXT("Describe a UE MCP tool by the registry_name returned from catalog search. Returns its input schema; no engine tool is executed."),
        {TEXT("registry_name")}, {TEXT("registry_name")}));
    Definitions.Add(MakeMetaDefinition(CatalogCallName, CatalogCallId,
        TEXT("Call an approved UE MCP tool by registry_name, using arguments that match its described schema. Every actual call requires explicit user approval."),
        {TEXT("registry_name")}, {TEXT("registry_name"), TEXT("arguments")}));
    return Definitions;
}

bool IsMetaName(const FString& Name)
{
    return Name == CatalogSearchName || Name == CatalogDescribeName || Name == CatalogCallName;
}

struct FCanonicalBinding
{
    const TCHAR* RegistryName;
    const TCHAR* NativeHandlerId;
};

// These names retain their published meaning even if MCPTools.json is reloaded.
// Other configured names may bind to any supported native handler.
constexpr FCanonicalBinding CanonicalBindings[] = {
    {TEXT("bsharness.editor_info"), TEXT("editor_info")},
    {TEXT("bsharness.project_info"), TEXT("project_info")},
    {TEXT("bsharness.assets.search"), TEXT("assets.search")},
    {TEXT("bsharness.assets.get"), TEXT("assets.get")},
    {TEXT("bsharness.assets.open"), TEXT("assets.open")},
    {TEXT("bsharness.actors.list"), TEXT("actors.list")},
    {TEXT("bsharness.source.list"), TEXT("source.list")},
    {TEXT("bsharness.source.read"), TEXT("source.read")},
    {TEXT("bsharness.blueprint.variables"), TEXT("blueprint.variables")},
};

bool IsSupportedHandler(const FString& HandlerId)
{
    for (const FCanonicalBinding& Binding : CanonicalBindings)
    {
        if (HandlerId.Equals(Binding.NativeHandlerId, ESearchCase::CaseSensitive))
        {
            return true;
        }
    }
    return false;
}

bool IsAssetPathHandler(const FString& HandlerId)
{
    return HandlerId == TEXT("assets.get") || HandlerId == TEXT("assets.open")
        || HandlerId == TEXT("blueprint.variables");
}

bool IsAllowedDefinition(const FMCPToolDefinition& Definition)
{
    if (!Definition.InputSchema.IsValid() || !IsSupportedHandler(Definition.NativeHandlerId)
        || FAgentToolBridge::ToModelName(Definition.Name).IsEmpty())
    {
        return false;
    }
    for (const FCanonicalBinding& Binding : CanonicalBindings)
    {
        if (Definition.Name.Equals(Binding.RegistryName, ESearchCase::CaseSensitive))
        {
            return Definition.NativeHandlerId.Equals(Binding.NativeHandlerId, ESearchCase::CaseSensitive);
        }
    }
    return true;
}

bool IsFrozenBinding(const FAgentRunInputSnapshot& Snapshot, const FMCPToolDefinition& Definition)
{
    if (!Snapshot.bToolListFrozen)
    {
        return true;
    }
    const FString* FrozenHandler = Snapshot.AllowedToolHandlerIds.Find(Definition.Name);
    return FrozenHandler && FrozenHandler->Equals(Definition.NativeHandlerId, ESearchCase::CaseSensitive)
        && Snapshot.AllowedToolNames.ContainsByPredicate([&Definition](const FString& Name)
        {
            return Name.Equals(Definition.Name, ESearchCase::CaseSensitive);
        });
}

bool IsWorldAssetClassPath(const FString& ClassPath)
{
    return ClassPath.Equals(TEXT("/Script/Engine.World"), ESearchCase::CaseSensitive);
}

bool IsGameSearchPath(const FString& Path)
{
    return Path == TEXT("/Game") || Path.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive);
}

bool SearchDefaultPath(const FMCPToolDefinition& Definition, FString& OutPath)
{
    OutPath = TEXT("/Game");
    const TSharedPtr<FJsonObject>* Properties = nullptr;
    const TSharedPtr<FJsonObject>* Rule = nullptr;
    if (!Definition.InputSchema->TryGetObjectField(TEXT("properties"), Properties)
        || !(*Properties)->TryGetObjectField(TEXT("path"), Rule))
    {
        return true;
    }
    FString ConfiguredDefault;
    (*Rule)->TryGetStringField(TEXT("default"), ConfiguredDefault);
    const TArray<TSharedPtr<FJsonValue>>* ConfiguredEnum = nullptr;
    if ((*Rule)->TryGetArrayField(TEXT("enum"), ConfiguredEnum))
    {
        FString FirstAllowed;
        for (const TSharedPtr<FJsonValue>& Value : *ConfiguredEnum)
        {
            if (!Value.IsValid() || Value->Type != EJson::String || !IsGameSearchPath(Value->AsString()))
            {
                continue;
            }
            if (Value->AsString() == ConfiguredDefault)
            {
                OutPath = ConfiguredDefault;
                return true;
            }
            if (FirstAllowed.IsEmpty()) { FirstAllowed = Value->AsString(); }
        }
        if (FirstAllowed.IsEmpty()) { return false; }
        OutPath = MoveTemp(FirstAllowed);
        return true;
    }
    if (IsGameSearchPath(ConfiguredDefault)) { OutPath = ConfiguredDefault; }
    return true;
}

FMCPToolResult CheckCallPolicy(const FAgentRunInputSnapshot& Snapshot,
    const FMCPToolDefinition& Definition, const TSharedRef<FJsonObject>& Arguments)
{
    if (!Snapshot.bToolListFrozen || !IsFrozenBinding(Snapshot, Definition))
    {
        return FMCPToolResult::ProtocolError(-32602, TEXT("Tool binding was not frozen as available when this run started."));
    }
    const FString& HandlerId = Definition.NativeHandlerId;
    if (HandlerId == TEXT("assets.search"))
    {
        FString Path;
        if (!SearchDefaultPath(Definition, Path))
        {
            return FMCPToolResult::ProtocolError(-32602, TEXT("Configured asset search has no permitted /Game path."));
        }
        if (Arguments->HasField(TEXT("path")) && !Arguments->TryGetStringField(TEXT("path"), Path))
        {
            return FMCPToolResult::ProtocolError(-32602, TEXT("path must be a string under /Game."));
        }
        if (!IsGameSearchPath(Path))
        {
            return FMCPToolResult::ProtocolError(-32602, TEXT("Asset searches are limited to /Game and its subfolders."));
        }
    }
    if (IsAssetPathHandler(HandlerId))
    {
        FString Path;
        FString Normalized;
        if (!Arguments->TryGetStringField(TEXT("object_path"), Path)
            || !FAgentAssetContextService::NormalizeObjectPath(Path, Normalized) || Path != Normalized)
        {
            return FMCPToolResult::ProtocolError(-32602, TEXT("object_path must be a canonical /Game asset object path."));
        }
        const FAgentCandidate* Candidate = nullptr;
        if (HandlerId == TEXT("assets.open"))
        {
            Candidate = Snapshot.IncludedAssets.FindByPredicate([&Path](const FAgentCandidate& Asset)
            {
                return Asset.bIncluded && Asset.ObjectPath.Equals(Path, ESearchCase::CaseSensitive);
            });
            if (!Candidate)
            {
                return FMCPToolResult::ProtocolError(-32602, TEXT("Opening an asset requires its path in this run's frozen candidate list."));
            }
            if (IsWorldAssetClassPath(Candidate->AssetClassPath))
            {
                return FMCPToolResult::ProtocolError(-32602, TEXT("Opening a level is not available through Agent Workbench."));
            }
        }
        const FAgentAssetLookupResult Found = FAgentAssetContextService::LookupAsset(FSoftObjectPath(Path));
        if (Found.Status == EAgentCandidateValidation::Unknown)
        {
            return FMCPToolResult::Failure(TEXT("Asset Registry is still scanning; retry after scanning completes."));
        }
        if (Found.Status != EAgentCandidateValidation::Valid || !Found.Data.IsValid()
            || Found.Data.GetSoftObjectPath().ToString() != Path || Found.Data.IsRedirector())
        {
            return FMCPToolResult::Failure(TEXT("The /Game asset is unavailable or is a redirector."));
        }
        if (HandlerId == TEXT("blueprint.variables")
            && !Found.Data.IsInstanceOf(UBlueprint::StaticClass(), EResolveClass::Yes))
        {
            return FMCPToolResult::ProtocolError(-32602, TEXT("blueprint.variables requires a Blueprint asset."));
        }
        if (HandlerId == TEXT("assets.open") && Candidate->AssetClassPath != Found.Data.AssetClassPath.ToString())
        {
            return FMCPToolResult::Failure(TEXT("The attached asset's type has changed since this run started."));
        }
        if (HandlerId == TEXT("assets.open") && Found.Data.IsInstanceOf(UWorld::StaticClass(), EResolveClass::Yes))
        {
            return FMCPToolResult::ProtocolError(-32602, TEXT("Opening a level is not available through Agent Workbench."));
        }
    }
    return FMCPToolResult();
}

TArray<FMCPToolDefinition> FilterDefinitions(const FAgentRunInputSnapshot& Snapshot,
    TArray<FMCPToolDefinition> Definitions)
{
    TArray<FMCPToolDefinition> Available;
    for (FMCPToolDefinition& Definition : Definitions)
    {
        if (!IsAllowedDefinition(Definition) || !IsFrozenBinding(Snapshot, Definition))
        {
            continue;
        }
        const FString& HandlerId = Definition.NativeHandlerId;
        if (HandlerId == TEXT("assets.open"))
        {
            TArray<TSharedPtr<FJsonValue>> CandidatePaths;
            for (const FAgentCandidate& Candidate : Snapshot.IncludedAssets)
            {
                if (Candidate.bIncluded && !IsWorldAssetClassPath(Candidate.AssetClassPath))
                {
                    CandidatePaths.Add(MakeShared<FJsonValueString>(Candidate.ObjectPath));
                }
            }
            if (CandidatePaths.IsEmpty())
            {
                continue;
            }
            // Registry definitions share their JSON objects. Each request gets its own
            // schema so one run's candidate enum cannot change another run's tool.
            TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
            FJsonObject::Duplicate(TSharedPtr<const FJsonObject>(Definition.InputSchema), Schema);
            const TSharedPtr<FJsonObject>* Properties = nullptr;
            const TSharedPtr<FJsonObject>* ObjectPathRule = nullptr;
            if (!Schema || !Schema->TryGetObjectField(TEXT("properties"), Properties)
                || !(*Properties)->TryGetObjectField(TEXT("object_path"), ObjectPathRule))
            {
                continue;
            }
            const TArray<TSharedPtr<FJsonValue>>* ConfiguredEnum = nullptr;
            if ((*ObjectPathRule)->TryGetArrayField(TEXT("enum"), ConfiguredEnum))
            {
                CandidatePaths.RemoveAll([ConfiguredEnum](const TSharedPtr<FJsonValue>& Path)
                {
                    return !ConfiguredEnum->ContainsByPredicate([&Path](const TSharedPtr<FJsonValue>& Allowed)
                    {
                        return Allowed.IsValid() && Allowed->Type == EJson::String
                            && Allowed->AsString() == Path->AsString();
                    });
                });
                if (CandidatePaths.IsEmpty()) { continue; }
            }
            (*ObjectPathRule)->SetArrayField(TEXT("enum"), MoveTemp(CandidatePaths));
            (*ObjectPathRule)->SetStringField(TEXT("description"),
                TEXT("Exact /Game/Folder/Asset.Asset object path from this run's attached candidates."));
            Definition.InputSchema = MoveTemp(Schema);
            Definition.Description = TEXT("Open an attached non-level /Game asset in Unreal Editor, or focus its existing editor. This changes the editor UI.");
        }
        else if (HandlerId == TEXT("assets.get") || HandlerId == TEXT("blueprint.variables"))
        {
            Definition.Description += TEXT(" Accepts valid /Game object paths, including assets found by assets.search.");
        }
        else if (HandlerId == TEXT("assets.search"))
        {
            FString DefaultPath;
            if (!SearchDefaultPath(Definition, DefaultPath)) { continue; }
            Definition.Description += TEXT(" Search is limited to /Game and its subfolders.");
            TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
            FJsonObject::Duplicate(TSharedPtr<const FJsonObject>(Definition.InputSchema), Schema);
            const TSharedPtr<FJsonObject>* Properties = nullptr;
            const TSharedPtr<FJsonObject>* PathRule = nullptr;
            if (Schema && Schema->TryGetObjectField(TEXT("properties"), Properties)
                && (*Properties)->TryGetObjectField(TEXT("path"), PathRule))
            {
                (*PathRule)->SetStringField(TEXT("default"), DefaultPath);
                Definition.InputSchema = MoveTemp(Schema);
            }
        }
        Available.Add(MoveTemp(Definition));
    }
    // A provider must never receive two distinct registrations under one name.
    // Full MD5 suffixes make this unlikely, but fail closed if it ever happens.
    TSet<FString> SeenAliases;
    TSet<FString> ConflictingAliases;
    for (const FMCPToolDefinition& Definition : Available)
    {
        const FString Alias = FAgentToolBridge::ToModelName(Definition.Name);
        if (SeenAliases.Contains(Alias)) { ConflictingAliases.Add(Alias); }
        else { SeenAliases.Add(Alias); }
    }
    Available.RemoveAll([&ConflictingAliases](const FMCPToolDefinition& Definition)
    {
        return ConflictingAliases.Contains(FAgentToolBridge::ToModelName(Definition.Name));
    });
    return Available;
}

void RemoveNameAndAliasConflicts(TArray<FMCPToolDefinition>& Definitions)
{
    TSet<FString> SeenNames;
    TSet<FString> DuplicateNames;
    TSet<FString> SeenAliases;
    TSet<FString> DuplicateAliases;
    for (const FMCPToolDefinition& Definition : Definitions)
    {
        if (SeenNames.Contains(Definition.Name)) { DuplicateNames.Add(Definition.Name); }
        else { SeenNames.Add(Definition.Name); }
        const FString Alias = FAgentToolBridge::ToModelName(Definition.Name);
        if (Alias.IsEmpty() || SeenAliases.Contains(Alias)) { DuplicateAliases.Add(Alias); }
        else { SeenAliases.Add(Alias); }
    }
    Definitions.RemoveAll([&](const FMCPToolDefinition& Definition)
    {
        return DuplicateNames.Contains(Definition.Name)
            || DuplicateAliases.Contains(FAgentToolBridge::ToModelName(Definition.Name));
    });
}
}

TArray<FMCPToolDefinition> FAgentToolBridge::ListAvailable(const FAgentRunInputSnapshot& Snapshot)
{
    check(IsInGameThread());
    TArray<FMCPToolDefinition> Available = FilterDefinitions(Snapshot,
        FBSHarnessToolsModule::Get().GetToolRegistry().ListTools());
    for (FMCPToolDefinition& Meta : MetaDefinitions())
    {
        if (IsFrozenBinding(Snapshot, Meta)) { Available.Add(MoveTemp(Meta)); }
    }
    // UE schemas remain in the frozen local catalog and are fetched through
    // catalog.describe. Keep provider requests stable across model vendors.
    RemoveNameAndAliasConflicts(Available);
    return Available;
}

void FAgentToolBridge::FreezeAvailableTools(FAgentRunInputSnapshot& Snapshot)
{
    check(IsInGameThread());
    check(!Snapshot.bToolListFrozen);
    Snapshot.AllowedToolNames.Reset();
    Snapshot.AllowedToolHandlerIds.Reset();
    TArray<FMCPToolDefinition> All = FilterDefinitions(Snapshot,
        FBSHarnessToolsModule::Get().GetToolRegistry().ListTools());
    All.Append(MetaDefinitions());
    All.Append(FAgentUnrealMCPBridge::ListAvailable());
    RemoveNameAndAliasConflicts(All);
    for (const FMCPToolDefinition& Definition : All)
    {
        Snapshot.AllowedToolNames.Add(Definition.Name);
        Snapshot.AllowedToolHandlerIds.Add(Definition.Name, Definition.NativeHandlerId);
    }
}

bool FAgentToolBridge::ResolveApprovalTarget(const FAgentRunInputSnapshot& Snapshot,
    const FString& RegistryName, const TSharedRef<FJsonObject>& Arguments,
    FString& OutTargetRegistryName, TSharedPtr<FJsonObject>& OutTargetArguments, FString& OutError)
{
    check(IsInGameThread());
    OutTargetRegistryName.Reset();
    OutTargetArguments.Reset();
    OutError.Reset();
    if (!Snapshot.bToolListFrozen || !Snapshot.AllowedToolNames.ContainsByPredicate([&](const FString& Name)
        { return Name.Equals(RegistryName, ESearchCase::CaseSensitive); }))
    {
        OutError = TEXT("Tool was not frozen as available when this run started.");
        return false;
    }
    const FString* FrozenHandler = Snapshot.AllowedToolHandlerIds.Find(RegistryName);
    if (!FrozenHandler)
    {
        OutError = TEXT("Tool has no frozen handler identity.");
        return false;
    }
    FString TargetName = RegistryName;
    TSharedPtr<FJsonObject> TargetArguments = Arguments;
    if (RegistryName == CatalogCallName)
    {
        const TSharedPtr<FJsonObject>* NestedArguments = nullptr;
        if (*FrozenHandler != CatalogCallId || Arguments->Values.Num() != 2
            || !Arguments->TryGetStringField(TEXT("registry_name"), TargetName)
            || TargetName.IsEmpty()
            || !Arguments->TryGetObjectField(TEXT("arguments"), NestedArguments)
            || !NestedArguments || !NestedArguments->IsValid())
        {
            OutError = TEXT("ue_mcp.call_tool requires exactly registry_name and arguments object.");
            return false;
        }
        TargetArguments = *NestedArguments;
        if (!FAgentUnrealMCPBridge::IsUnrealToolName(TargetName) || IsMetaName(TargetName))
        {
            OutError = TEXT("ue_mcp.call_tool can target only a discovered UE MCP tool.");
            return false;
        }
    }
    const FString* TargetHandler = Snapshot.AllowedToolHandlerIds.Find(TargetName);
    if (!TargetHandler || !Snapshot.AllowedToolNames.ContainsByPredicate([&](const FString& Name)
        { return Name.Equals(TargetName, ESearchCase::CaseSensitive); }))
    {
        OutError = TEXT("Target tool was not frozen as available when this run started.");
        return false;
    }
    if (FAgentUnrealMCPBridge::IsUnrealToolName(TargetName) && !IsMetaName(TargetName)
        && !FAgentUnrealMCPBridge::ValidateArguments(TargetName, *TargetHandler,
            TargetArguments.ToSharedRef(), OutError))
    {
        return false;
    }
    if (IsMetaName(TargetName))
    {
        const TCHAR* Expected = TargetName == CatalogSearchName ? CatalogSearchId
            : TargetName == CatalogDescribeName ? CatalogDescribeId : CatalogCallId;
        if (*TargetHandler != Expected)
        {
            OutError = TEXT("Catalog tool identity changed since this run started.");
            return false;
        }
    }
    OutTargetRegistryName = MoveTemp(TargetName);
    OutTargetArguments = MoveTemp(TargetArguments);
    return true;
}

FString FAgentToolBridge::GetDisplayName(const FAgentRunInputSnapshot& Snapshot, const FString& RegistryName)
{
    check(IsInGameThread());
    const FString* FrozenHandler = Snapshot.AllowedToolHandlerIds.Find(RegistryName);
    if (FrozenHandler && FAgentUnrealMCPBridge::IsUnrealToolName(RegistryName)
        && !IsMetaName(RegistryName))
    {
        return FAgentUnrealMCPBridge::DisplayName(RegistryName, *FrozenHandler);
    }
    return RegistryName;
}

bool FAgentToolBridge::RequiresApproval(const FAgentRunInputSnapshot& Snapshot, const FString& RegistryName)
{
    check(IsInGameThread());
    if (!Snapshot.bToolListFrozen) { return true; }
    const FString* FrozenHandler = Snapshot.AllowedToolHandlerIds.Find(RegistryName);
    if (!FrozenHandler || !Snapshot.AllowedToolNames.ContainsByPredicate([&](const FString& Name)
        { return Name.Equals(RegistryName, ESearchCase::CaseSensitive); }))
    {
        return true;
    }
    if (IsMetaName(RegistryName))
    {
        const TCHAR* Expected = RegistryName == CatalogSearchName ? CatalogSearchId
            : RegistryName == CatalogDescribeName ? CatalogDescribeId : CatalogCallId;
        return *FrozenHandler != Expected || RegistryName == CatalogCallName;
    }
    const TArray<FMCPToolDefinition> Current = FBSHarnessToolsModule::Get().GetToolRegistry().ListTools();
    const FMCPToolDefinition* Native = Current.FindByPredicate([&](const FMCPToolDefinition& Definition)
    {
        return Definition.Name.Equals(RegistryName, ESearchCase::CaseSensitive);
    });
    if (Native)
    {
        return !IsAllowedDefinition(*Native) || Native->NativeHandlerId != *FrozenHandler
            || Native->NativeHandlerId == TEXT("assets.open");
    }
    return !FAgentUnrealMCPBridge::IsUnrealToolName(RegistryName)
        || FAgentUnrealMCPBridge::RequiresApproval(RegistryName, *FrozenHandler);
}

void FAgentToolBridge::Execute(const FAgentRunInputSnapshot& Snapshot, const FString& RegistryName,
    const TSharedRef<FJsonObject>& Arguments, double TimeoutSeconds, bool bApproved,
    TFunction<void(FMCPToolResult)> Completion)
{
    check(IsInGameThread());
    check(Completion);
    if (!Snapshot.bToolListFrozen || !Snapshot.AllowedToolNames.ContainsByPredicate([&](const FString& Name)
        { return Name.Equals(RegistryName, ESearchCase::CaseSensitive); }))
    {
        Completion(FMCPToolResult::ProtocolError(-32602, TEXT("Tool was not frozen as available when this run started.")));
        return;
    }
    const FString* FrozenHandler = Snapshot.AllowedToolHandlerIds.Find(RegistryName);
    if (!FrozenHandler)
    {
        Completion(FMCPToolResult::ProtocolError(-32602, TEXT("Tool has no frozen handler identity.")));
        return;
    }
    if (IsMetaName(RegistryName))
    {
        FString TargetName;
        TSharedPtr<FJsonObject> TargetArguments;
        FString Error;
        if (!ResolveApprovalTarget(Snapshot, RegistryName, Arguments,
            TargetName, TargetArguments, Error))
        {
            Completion(FMCPToolResult::ProtocolError(-32602, Error));
            return;
        }
        if (RegistryName == CatalogCallName)
        {
            if (!bApproved)
            {
                Completion(FMCPToolResult::ProtocolError(-32602, TEXT("UE MCP tool requires explicit user approval.")));
                return;
            }
            const FString* TargetHandler = Snapshot.AllowedToolHandlerIds.Find(TargetName);
            FAgentUnrealMCPBridge::Execute(TargetName, *TargetHandler,
                TargetArguments.ToSharedRef(), TimeoutSeconds, true, MoveTemp(Completion));
            return;
        }
        if (RegistryName == CatalogDescribeName)
        {
            FString Requested;
            if (Arguments->Values.Num() != 1 || !Arguments->TryGetStringField(TEXT("registry_name"), Requested))
            {
                Completion(FMCPToolResult::ProtocolError(-32602, TEXT("registry_name is required.")));
                return;
            }
            Completion(FAgentUnrealMCPBridge::DescribeCatalogTool(Snapshot.AllowedToolHandlerIds, Requested));
            return;
        }
        FString Query;
        if (Arguments->HasField(TEXT("query")) && !Arguments->TryGetStringField(TEXT("query"), Query))
        {
            Completion(FMCPToolResult::ProtocolError(-32602, TEXT("query must be a string.")));
            return;
        }
        auto IntegerArgument = [&](const TCHAR* Name, int32 Default, int32& Out) -> bool
        {
            Out = Default;
            if (!Arguments->HasField(Name)) { return true; }
            double Value = 0;
            if (!Arguments->TryGetNumberField(Name, Value) || !FMath::IsFinite(Value)
                || Value != FMath::FloorToDouble(Value) || Value < 0 || Value > MAX_int32)
            {
                return false;
            }
            Out = static_cast<int32>(Value);
            return true;
        };
        int32 Offset;
        int32 Limit;
        if (!IntegerArgument(TEXT("offset"), 0, Offset) || !IntegerArgument(TEXT("limit"), 50, Limit))
        {
            Completion(FMCPToolResult::ProtocolError(-32602, TEXT("offset and limit must be integers.")));
            return;
        }
        Completion(FAgentUnrealMCPBridge::SearchCatalog(Snapshot.AllowedToolHandlerIds, Query, Offset, Limit));
        return;
    }
    FMCPToolRegistry& Registry = FBSHarnessToolsModule::Get().GetToolRegistry();
    const TArray<FMCPToolDefinition> Current = Registry.ListTools();
    const FMCPToolDefinition* Definition = Current.FindByPredicate([&RegistryName](const FMCPToolDefinition& Candidate)
    {
        return Candidate.Name.Equals(RegistryName, ESearchCase::CaseSensitive);
    });
    if (!Definition && FAgentUnrealMCPBridge::IsUnrealToolName(RegistryName))
    {
        FAgentUnrealMCPBridge::Execute(RegistryName, *FrozenHandler, Arguments,
            TimeoutSeconds, bApproved, MoveTemp(Completion));
        return;
    }
    if (!Definition || !IsAllowedDefinition(*Definition))
    {
        Completion(FMCPToolResult::ProtocolError(-32602, TEXT("Tool is unavailable or not bound to an approved native handler.")));
        return;
    }
    const FString ModelAlias = ToModelName(RegistryName);
    if (Current.ContainsByPredicate([&](const FMCPToolDefinition& Other)
    {
        return Other.Name != RegistryName && IsAllowedDefinition(Other)
            && ToModelName(Other.Name) == ModelAlias;
    }))
    {
        Completion(FMCPToolResult::ProtocolError(-32602, TEXT("Tool has a conflicting model alias.")));
        return;
    }
    const FMCPToolResult Policy = CheckCallPolicy(Snapshot, *Definition, Arguments);
    if (Policy.bIsError)
    {
        Completion(Policy);
        return;
    }
    if (Definition->NativeHandlerId == TEXT("assets.open") && !bApproved)
    {
        Completion(FMCPToolResult::ProtocolError(-32602, TEXT("Opening an asset requires explicit user approval.")));
        return;
    }
    TSharedRef<FJsonObject> SafeArguments = Arguments;
    if (Definition->NativeHandlerId == TEXT("assets.search") && !Arguments->HasField(TEXT("path")))
    {
        FString DefaultPath;
        if (!SearchDefaultPath(*Definition, DefaultPath))
        {
            Completion(FMCPToolResult::ProtocolError(-32602, TEXT("Configured asset search has no permitted /Game path.")));
            return;
        }
        TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>();
        FJsonObject::Duplicate(TSharedPtr<const FJsonObject>(Arguments), Copy);
        SafeArguments = Copy.ToSharedRef();
        SafeArguments->SetStringField(TEXT("path"), DefaultPath);
    }
    // A synchronous handler may resolve before CallTool returns. Async source
    // handlers can resolve on a worker, so carry only values into the continuation.
    Registry.CallTool(RegistryName, SafeArguments, TimeoutSeconds).Next(
        [Completion = MoveTemp(Completion)](FMCPToolResult Result) mutable
        {
            if (IsInGameThread())
            {
                Completion(MoveTemp(Result));
            }
            else
            {
                AsyncTask(ENamedThreads::GameThread,
                    [Completion = MoveTemp(Completion), Result = MoveTemp(Result)]() mutable
                    {
                        Completion(MoveTemp(Result));
                    });
            }
        });
}

FString FAgentToolBridge::ToModelName(const FString& RegistryName)
{
    if (RegistryName.IsEmpty() || RegistryName.Len() > 128) { return FString(); }
    FString Readable;
    Readable.Reserve(27);
    for (const TCHAR Character : RegistryName)
    {
        const bool bAlphaNumeric = (Character >= TEXT('a') && Character <= TEXT('z'))
            || (Character >= TEXT('A') && Character <= TEXT('Z'))
            || (Character >= TEXT('0') && Character <= TEXT('9'));
        if (!bAlphaNumeric && Character != TEXT('_') && Character != TEXT('-') && Character != TEXT('.'))
        {
            return FString();
        }
        if (Readable.Len() < 27)
        {
            Readable.AppendChar(Character == TEXT('.') ? TEXT('_') : Character);
        }
    }
    return TEXT("bsh_") + Readable + TEXT("_") + FMD5::HashAnsiString(*RegistryName);
}

FString FAgentToolBridge::FromModelName(const FString& ModelName)
{
    check(IsInGameThread());
    FString Match;
    for (const FMCPToolDefinition& Definition : ListAvailable(FAgentRunInputSnapshot()))
    {
        if (ToModelName(Definition.Name) == ModelName)
        {
            if (!Match.IsEmpty()) { return FString(); }
            Match = Definition.Name;
        }
    }
    return Match;
}

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAgentToolBridgePolicyTest,
    "AgentWorkbench.ToolsBridge.RegisteredToolsPolicy",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAgentToolBridgePolicyTest::RunTest(const FString& Parameters)
{
    auto Definition = [](const TCHAR* Name, const TCHAR* Handler)
    {
        FMCPToolDefinition Tool;
        Tool.Name = Name;
        Tool.NativeHandlerId = Handler;
        Tool.InputSchema = MakeShared<FJsonObject>();
        const TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
        Properties->SetObjectField(TEXT("object_path"), MakeShared<FJsonObject>());
        Properties->SetObjectField(TEXT("path"), MakeShared<FJsonObject>());
        Tool.InputSchema->SetObjectField(TEXT("properties"), Properties);
        return Tool;
    };

    FAgentRunInputSnapshot Snapshot;
    TArray<FMCPToolDefinition> Configured;
    Configured.Add(Definition(TEXT("bsharness.editor_info"), TEXT("editor_info")));
    Configured.Add(Definition(TEXT("bsharness.project_info"), TEXT("assets.open")));
    Configured.Add(Definition(TEXT("bsharness.assets.open"), TEXT("assets.open")));
    Configured.Add(Definition(TEXT("bsharness.assets.get"), TEXT("assets.get")));
    Configured.Add(Definition(TEXT("bsharness.blueprint.variables"), TEXT("blueprint.variables")));
    Configured.Add(Definition(TEXT("custom.source_reader"), TEXT("source.read")));
    Configured.Add(Definition(TEXT("custom.unknown"), TEXT("unknown.handler")));
    const TArray<FMCPToolDefinition> WithoutAssets = FilterDefinitions(Snapshot, Configured);
    TestEqual(TEXT("Read-only asset tools are available without candidates"), WithoutAssets.Num(), 4);
    TestTrue(TEXT("Blueprint variables can be offered before asset search"),
        WithoutAssets.ContainsByPredicate([](const FMCPToolDefinition& Tool)
        {
            return Tool.NativeHandlerId == TEXT("blueprint.variables");
        }));
    TestTrue(TEXT("Custom configured alias is exposed"), WithoutAssets.ContainsByPredicate([](const FMCPToolDefinition& Tool)
    {
        return Tool.Name == TEXT("custom.source_reader");
    }));

    FMCPToolDefinition RestrictedSearch = Definition(TEXT("bsharness.assets.search"), TEXT("assets.search"));
    const auto SearchRule = RestrictedSearch.InputSchema->GetObjectField(TEXT("properties"))->GetObjectField(TEXT("path"));
    TArray<TSharedPtr<FJsonValue>> SearchPaths;
    SearchPaths.Add(MakeShared<FJsonValueString>(TEXT("/Engine")));
    SearchPaths.Add(MakeShared<FJsonValueString>(TEXT("/Game/Subdir")));
    SearchRule->SetArrayField(TEXT("enum"), SearchPaths);
    SearchRule->SetStringField(TEXT("default"), TEXT("/Engine"));
    FString SearchPath;
    TestTrue(TEXT("Search finds a safe configured enum value"), SearchDefaultPath(RestrictedSearch, SearchPath));
    TestEqual(TEXT("Search does not inject a path outside the configured enum"), SearchPath, FString(TEXT("/Game/Subdir")));
    SearchPaths.SetNum(1);
    SearchRule->SetArrayField(TEXT("enum"), SearchPaths);
    TestFalse(TEXT("Search with no /Game enum value is not offered"),
        SearchDefaultPath(RestrictedSearch, SearchPath));

    FAgentCandidate Candidate;
    Candidate.ObjectPath = TEXT("/Game/Test.Test");
    Candidate.AssetClassPath = TEXT("/Script/Engine.Blueprint");
    Snapshot.IncludedAssets.Add(Candidate);
    const TArray<FMCPToolDefinition> WithAssets = FilterDefinitions(Snapshot, Configured);
    TestEqual(TEXT("Asset open, get and blueprint variables are registered"), WithAssets.Num(), 5);
    const FMCPToolDefinition* Open = WithAssets.FindByPredicate([](const FMCPToolDefinition& Tool)
    {
        return Tool.NativeHandlerId == TEXT("assets.open");
    });
    TestTrue(TEXT("Asset open has candidate schema"), Open != nullptr);
    if (Open)
    {
        const auto Rule = Open->InputSchema->GetObjectField(TEXT("properties"))->GetObjectField(TEXT("object_path"));
        const TArray<TSharedPtr<FJsonValue>>* EnumValues = nullptr;
        TestTrue(TEXT("Candidate path is in offered schema"), Rule->TryGetArrayField(TEXT("enum"), EnumValues)
            && EnumValues && EnumValues->Num() == 1 && (*EnumValues)[0]->AsString() == Candidate.ObjectPath);
    }
    const auto OriginalRule = Configured[2].InputSchema->GetObjectField(TEXT("properties"))->GetObjectField(TEXT("object_path"));
    TestFalse(TEXT("Candidate enum does not mutate registry schema"), OriginalRule->HasField(TEXT("enum")));
    TArray<FMCPToolDefinition> Restricted = Configured;
    TSharedPtr<FJsonObject> RestrictedSchema = MakeShared<FJsonObject>();
    FJsonObject::Duplicate(TSharedPtr<const FJsonObject>(Restricted[2].InputSchema), RestrictedSchema);
    Restricted[2].InputSchema = RestrictedSchema;
    TArray<TSharedPtr<FJsonValue>> ConfiguredPaths;
    ConfiguredPaths.Add(MakeShared<FJsonValueString>(TEXT("/Game/Different.Different")));
    RestrictedSchema->GetObjectField(TEXT("properties"))->GetObjectField(TEXT("object_path"))->SetArrayField(
        TEXT("enum"), MoveTemp(ConfiguredPaths));
    TestFalse(TEXT("Candidate restrictions do not widen a configured enum"),
        FilterDefinitions(Snapshot, Restricted).ContainsByPredicate([](const FMCPToolDefinition& Tool)
        {
            return Tool.NativeHandlerId == TEXT("assets.open");
        }));

    FAgentRunInputSnapshot WorldSnapshot;
    FAgentCandidate WorldCandidate;
    WorldCandidate.ObjectPath = TEXT("/Game/Maps/TestMap.TestMap");
    WorldCandidate.AssetClassPath = TEXT("/Script/Engine.World");
    WorldSnapshot.IncludedAssets.Add(WorldCandidate);
    const TArray<FMCPToolDefinition> WithWorld = FilterDefinitions(WorldSnapshot, Configured);
    TestFalse(TEXT("Level open is not offered"), WithWorld.ContainsByPredicate([](const FMCPToolDefinition& Tool)
    {
        return Tool.NativeHandlerId == TEXT("assets.open");
    }));
    TestTrue(TEXT("Asset metadata remains available for world assets"), WithWorld.ContainsByPredicate([](const FMCPToolDefinition& Tool)
    {
        return Tool.NativeHandlerId == TEXT("assets.get");
    }));

    Snapshot.bToolListFrozen = true;
    Snapshot.AllowedToolNames.Add(TEXT("bsharness.assets.open"));
    Snapshot.AllowedToolHandlerIds.Add(TEXT("bsharness.assets.open"), TEXT("assets.open"));
    TestEqual(TEXT("Frozen binding offers only its approved tool"), FilterDefinitions(Snapshot, Configured).Num(), 1);
    Snapshot.AllowedToolHandlerIds[TEXT("bsharness.assets.open")] = TEXT("assets.get");
    TestTrue(TEXT("Configuration remapping after freeze is rejected"), FilterDefinitions(Snapshot, Configured).IsEmpty());

    FAgentRunInputSnapshot CallSnapshot;
    CallSnapshot.bToolListFrozen = true;
    CallSnapshot.AllowedToolNames.Add(TEXT("bsharness.assets.search"));
    CallSnapshot.AllowedToolHandlerIds.Add(TEXT("bsharness.assets.search"), TEXT("assets.search"));
    CallSnapshot.AllowedToolNames.Add(TEXT("bsharness.assets.get"));
    CallSnapshot.AllowedToolHandlerIds.Add(TEXT("bsharness.assets.get"), TEXT("assets.get"));
    CallSnapshot.AllowedToolNames.Add(TEXT("bsharness.assets.open"));
    CallSnapshot.AllowedToolHandlerIds.Add(TEXT("bsharness.assets.open"), TEXT("assets.open"));
    auto Args = MakeShared<FJsonObject>();
    Args->SetStringField(TEXT("path"), TEXT("/Engine"));
    TestTrue(TEXT("Search outside /Game rejected"),
        CheckCallPolicy(CallSnapshot, Definition(TEXT("bsharness.assets.search"), TEXT("assets.search")), Args).bIsError);
    Args->SetStringField(TEXT("path"), TEXT("/Game/Maps"));
    TestFalse(TEXT("Search in a /Game subfolder permitted"),
        CheckCallPolicy(CallSnapshot, Definition(TEXT("bsharness.assets.search"), TEXT("assets.search")), Args).bIsError);
    Args = MakeShared<FJsonObject>();
    Args->SetStringField(TEXT("object_path"), TEXT("/Engine/Other.Other"));
    TestTrue(TEXT("Asset read outside /Game rejected"),
        CheckCallPolicy(CallSnapshot, Definition(TEXT("bsharness.assets.get"), TEXT("assets.get")), Args).bIsError);
    Args->SetStringField(TEXT("object_path"), TEXT("/Game/Other.Other"));
    TestTrue(TEXT("Asset open without a candidate rejected"),
        CheckCallPolicy(CallSnapshot, Definition(TEXT("bsharness.assets.open"), TEXT("assets.open")), Args).bIsError);

    const FString Name = FAgentToolBridge::ToModelName(TEXT("bsharness.source.read"));
    TestTrue(TEXT("Provider-safe alias has maximum length 64"), Name.Len() <= 64 && Name.StartsWith(TEXT("bsh_")));
    for (const TCHAR Character : Name)
    {
        const bool bAllowed = (Character >= TEXT('a') && Character <= TEXT('z'))
            || (Character >= TEXT('A') && Character <= TEXT('Z'))
            || (Character >= TEXT('0') && Character <= TEXT('9'))
            || Character == TEXT('_') || Character == TEXT('-');
        TestTrue(TEXT("Provider-safe alias uses supported characters"), bAllowed);
    }
    TestNotEqual(TEXT("Sanitization collisions have distinct aliases"),
        FAgentToolBridge::ToModelName(TEXT("custom.foo.bar")),
        FAgentToolBridge::ToModelName(TEXT("custom.foo_bar")));
    TestEqual(TEXT("Very long configured name remains provider-safe"),
        FAgentToolBridge::ToModelName(FString::ChrN(128, TEXT('x'))).Len(), 64);
    TestTrue(TEXT("Invalid configured name has no alias"), FAgentToolBridge::ToModelName(TEXT("bad name")).IsEmpty());

    struct FSearchObservation
    {
        bool bDone = false;
        FMCPToolResult Result;
    };
    const TSharedRef<FSearchObservation> Search = MakeShared<FSearchObservation>();
    FAgentToolBridge::Execute(CallSnapshot, TEXT("bsharness.assets.search"),
        MakeShared<FJsonObject>(), 5.0, false,
        [Search](FMCPToolResult Result)
        {
            Search->bDone = true;
            Search->Result = MoveTemp(Result);
        });
    const double SearchDeadline = FPlatformTime::Seconds() + 6.0;
    ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Search, SearchDeadline]()
    {
        if (!Search->bDone && FPlatformTime::Seconds() < SearchDeadline) { return false; }
        TestTrue(TEXT("Search without path completed through default injection"), Search->bDone);
        if (Search->bDone)
        {
            TestFalse(TEXT("Default /Game search succeeded"), Search->Result.bIsError);
            TestTrue(TEXT("Default /Game search returned asset registry data"),
                Search->Result.StructuredContent.IsValid()
                && Search->Result.StructuredContent->HasField(TEXT("items")));
        }
        return true;
    }));
    return true;
}

#endif
