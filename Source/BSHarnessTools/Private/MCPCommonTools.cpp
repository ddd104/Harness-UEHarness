#include "MCPCommonTools.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Async/Async.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/Selection.h"
#include "EngineUtils.h"
#include "HAL/PlatformFileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/UnrealType.h"

namespace
{
	using FJson = TSharedRef<FJsonObject>;

	FJson Property(const TCHAR* Type, const TCHAR* Description)
	{
		auto Value = MakeShared<FJsonObject>();
		Value->SetStringField(TEXT("type"), Type);
		Value->SetStringField(TEXT("description"), Description);
		return Value;
	}

	void AddString(FJson Properties, const TCHAR* Name, const TCHAR* Description)
	{
		Properties->SetObjectField(Name, Property(TEXT("string"), Description));
	}

	void AddBool(FJson Properties, const TCHAR* Name, const TCHAR* Description, bool Default)
	{
		auto Value = Property(TEXT("boolean"), Description);
		Value->SetBoolField(TEXT("default"), Default);
		Properties->SetObjectField(Name, Value);
	}

	void AddInteger(FJson Properties, const TCHAR* Name, int32 Default, int32 Minimum, int32 Maximum)
	{
		auto Value = Property(TEXT("integer"), Name);
		Value->SetNumberField(TEXT("default"), Default);
		Value->SetNumberField(TEXT("minimum"), Minimum);
		Value->SetNumberField(TEXT("maximum"), Maximum);
		Properties->SetObjectField(Name, Value);
	}

	FMCPToolDefinition Define(const TCHAR* Name, const TCHAR* Description, FJson Properties,
		std::initializer_list<const TCHAR*> Required = {})
	{
		FMCPToolDefinition Definition;
		Definition.Name = Name;
		Definition.Description = Description;
		Definition.NativeHandlerId = Name;
		Definition.InputSchema = MakeShared<FJsonObject>();
		Definition.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		Definition.InputSchema->SetObjectField(TEXT("properties"), Properties);
		Definition.InputSchema->SetBoolField(TEXT("additionalProperties"), false);
		TArray<TSharedPtr<FJsonValue>> Fields;
		for (const TCHAR* NameRequired : Required)
		{
			Fields.Add(MakeShared<FJsonValueString>(NameRequired));
		}
		Definition.InputSchema->SetArrayField(TEXT("required"), Fields);
		return Definition;
	}

	// Validate the small schema subset used by these built-ins, before executing a handler.
	FString Validate(const FMCPToolArguments& Arguments, const FJson& Schema, bool bCheckRequired = true)
	{
		const auto Properties = Schema->GetObjectField(TEXT("properties"));
		for (const auto& Required : Schema->GetArrayField(TEXT("required")))
		{
			if (bCheckRequired && !Arguments->HasField(Required->AsString()))
			{
				return FString::Printf(TEXT("Missing argument: %s"), *Required->AsString());
			}
		}
		for (const auto& Pair : Arguments->Values)
		{
			const FString Key(Pair.Key);
			const TSharedPtr<FJsonObject>* Rule = nullptr;
			if (!Properties->TryGetObjectField(Key, Rule))
			{
				return FString::Printf(TEXT("Unknown argument: %s"), *Key);
			}
			const FString Type = (*Rule)->GetStringField(TEXT("type"));
			const auto& Value = Pair.Value;
			if (!Value.IsValid()
				|| (Type == TEXT("string") && Value->Type != EJson::String)
				|| (Type == TEXT("boolean") && Value->Type != EJson::Boolean)
				|| (Type == TEXT("integer") && (Value->Type != EJson::Number || !FMath::IsFinite(Value->AsNumber())
					|| FMath::FloorToDouble(Value->AsNumber()) != Value->AsNumber()
					|| Value->AsNumber() < (*Rule)->GetNumberField(TEXT("minimum"))
					|| Value->AsNumber() > (*Rule)->GetNumberField(TEXT("maximum")))))
			{
				return FString::Printf(TEXT("Invalid type or range for argument: %s"), *Key);
			}
			const TArray<TSharedPtr<FJsonValue>>* Allowed = nullptr;
			if ((*Rule)->TryGetArrayField(TEXT("enum"), Allowed)
				&& !Allowed->ContainsByPredicate([&](const auto& Item) { return Item->AsString().Equals(Value->AsString(), ESearchCase::CaseSensitive); }))
			{
				return FString::Printf(TEXT("Unsupported value for argument: %s"), *Key);
			}
		}
		return FString();
	}

	FMCPToolResult DataResult(FJson Data)
	{
		FString Text;
		FJsonSerializer::Serialize(Data, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
		return FMCPToolResult::Success(Text, Data);
	}

	FString StringArg(const FMCPToolArguments& Arguments, const TCHAR* Name, const TCHAR* Default = TEXT(""))
	{
		FString Value = Default;
		Arguments->TryGetStringField(Name, Value);
		return Value;
	}

	int32 IntArg(const FMCPToolArguments& Arguments, const TCHAR* Name, int32 Default)
	{
		return Arguments->HasField(Name) ? Arguments->GetIntegerField(Name) : Default;
	}

	bool BoolArg(const FMCPToolArguments& Arguments, const TCHAR* Name, bool Default)
	{
		return Arguments->HasField(Name) ? Arguments->GetBoolField(Name) : Default;
	}

	void AddPaging(FJson Properties)
	{
		AddInteger(Properties, TEXT("offset"), 0, 0, MAX_int32);
		AddInteger(Properties, TEXT("limit"), 100, 1, 500);
	}

	FJson Page(const TArray<TSharedPtr<FJsonValue>>& Items, int32 Offset, int32 Limit)
	{
		auto Data = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Values;
		const int32 End = static_cast<int32>(FMath::Min<int64>(Items.Num(), static_cast<int64>(Offset) + Limit));
		for (int32 Index = Offset; Index < End; ++Index)
		{
			Values.Add(Items[Index]);
		}
		Data->SetArrayField(TEXT("items"), Values);
		Data->SetNumberField(TEXT("total"), Items.Num());
		Data->SetNumberField(TEXT("offset"), Offset);
		Data->SetBoolField(TEXT("hasMore"), End < Items.Num());
		if (End < Items.Num())
		{
			Data->SetNumberField(TEXT("nextOffset"), End);
		}
		return Data;
	}

	FJson AssetJson(const FAssetData& Asset)
	{
		auto Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("objectPath"), Asset.GetSoftObjectPath().ToString());
		Data->SetStringField(TEXT("packageName"), Asset.PackageName.ToString());
		Data->SetStringField(TEXT("assetName"), Asset.AssetName.ToString());
		Data->SetStringField(TEXT("classPath"), Asset.AssetClassPath.ToString());
		return Data;
	}

	IAssetRegistry& Assets()
	{
		return FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	}

	FMCPToolResult SearchAssets(const FMCPToolArguments& Arguments)
	{
		FString Path = StringArg(Arguments, TEXT("path"), TEXT("/Game"));
		Path.RemoveFromEnd(TEXT("/"));
		if (!FPackageName::IsValidLongPackageName(Path / TEXT("_MCPPathProbe"), true))
		{
			return FMCPToolResult::ProtocolError(-32602, TEXT("path must be a mounted package folder such as /Game."));
		}
		FARFilter Filter;
		Filter.PackagePaths.Add(FName(Path));
		Filter.bRecursivePaths = BoolArg(Arguments, TEXT("recursive"), true);
		const FString ClassPath = StringArg(Arguments, TEXT("class_path"));
		if (!ClassPath.IsEmpty())
		{
			if (!FPackageName::IsValidObjectPath(ClassPath))
			{
				return FMCPToolResult::ProtocolError(-32602, TEXT("class_path must be an object path, e.g. /Script/Engine.Material."));
			}
			Filter.ClassPaths.Add(FTopLevelAssetPath(ClassPath));
		}
		TArray<FAssetData> Found;
		Assets().GetAssets(Filter, Found);
		const FString Name = StringArg(Arguments, TEXT("name_contains"));
		Found.RemoveAll([&](const FAssetData& Asset) { return !Asset.AssetName.ToString().Contains(Name); });
		Found.Sort([](const FAssetData& A, const FAssetData& B) { return A.GetSoftObjectPath().ToString() < B.GetSoftObjectPath().ToString(); });
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const auto& Asset : Found)
		{
			Items.Add(MakeShared<FJsonValueObject>(AssetJson(Asset)));
		}
		auto Data = Page(Items, IntArg(Arguments, TEXT("offset"), 0), IntArg(Arguments, TEXT("limit"), 100));
		Data->SetBoolField(TEXT("isScanning"), Assets().IsLoadingAssets());
		return DataResult(Data);
	}

	FMCPToolResult GetAsset(const FMCPToolArguments& Arguments)
	{
		const FString Path = StringArg(Arguments, TEXT("object_path"));
		if (!FPackageName::IsValidObjectPath(Path) || !Path.Contains(TEXT(".")))
		{
			return FMCPToolResult::ProtocolError(-32602, TEXT("object_path must be a full asset object path, e.g. /Game/Folder/Asset.Asset."));
		}
		const FAssetData Asset = Assets().GetAssetByObjectPath(FSoftObjectPath(Path));
		if (!Asset.IsValid())
		{
			return FMCPToolResult::Failure(Assets().IsLoadingAssets()
				? TEXT("Asset not found; Asset Registry is still scanning. Retry after scanning completes.")
				: TEXT("Asset not found."));
		}
		auto Data = AssetJson(Asset);
		TArray<FName> Dependencies;
		Assets().GetDependencies(Asset.PackageName, Dependencies, UE::AssetRegistry::EDependencyCategory::Package);
		Dependencies.Sort(FNameLexicalLess());
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FName Dependency : Dependencies)
		{
			Values.Add(MakeShared<FJsonValueString>(Dependency.ToString()));
		}
		Data->SetArrayField(TEXT("dependencies"), Values);
		Data->SetBoolField(TEXT("isScanning"), Assets().IsLoadingAssets());
		return DataResult(Data);
	}

	FJson BlueprintVariableType(const FBPVariableDescription& Variable)
	{
		const FEdGraphPinType& Pin = Variable.VarType;
		auto Type = MakeShared<FJsonObject>();
		Type->SetStringField(TEXT("category"), Pin.PinCategory.ToString());
		if (!Pin.PinSubCategory.IsNone())
		{
			Type->SetStringField(TEXT("subcategory"), Pin.PinSubCategory.ToString());
		}
		if (const UObject* SubcategoryObject = Pin.PinSubCategoryObject.Get())
		{
			Type->SetStringField(TEXT("subcategory_object"), SubcategoryObject->GetPathName());
		}
		const TCHAR* Container = TEXT("none");
		switch (Pin.ContainerType)
		{
		case EPinContainerType::Array: Container = TEXT("array"); break;
		case EPinContainerType::Set: Container = TEXT("set"); break;
		case EPinContainerType::Map: Container = TEXT("map"); break;
		default: break;
		}
		Type->SetStringField(TEXT("container"), Container);
		if (Pin.IsMap())
		{
			Type->SetStringField(TEXT("value_category"), Pin.PinValueType.TerminalCategory.ToString());
			if (const UObject* ValueObject = Pin.PinValueType.TerminalSubCategoryObject.Get())
			{
				Type->SetStringField(TEXT("value_subcategory_object"), ValueObject->GetPathName());
			}
		}
		return Type;
	}

	FMCPToolResult GetBlueprintVariables(const FMCPToolArguments& Arguments)
	{
		check(IsInGameThread());
		const FString Path = StringArg(Arguments, TEXT("object_path"));
		if (!Path.StartsWith(TEXT("/Game/"), ESearchCase::CaseSensitive)
			|| !FPackageName::IsValidObjectPath(Path) || !Path.Contains(TEXT(".")) || Path.Contains(TEXT(":")))
		{
			return FMCPToolResult::ProtocolError(-32602, TEXT("object_path must be a canonical /Game Blueprint asset path, e.g. /Game/Folder/BP_Name.BP_Name."));
		}
		const FAssetData AssetData = Assets().GetAssetByObjectPath(FSoftObjectPath(Path));
		if (!AssetData.IsValid())
		{
			return FMCPToolResult::Failure(Assets().IsLoadingAssets()
				? TEXT("Blueprint asset not found while the Asset Registry is scanning; retry after scanning completes.")
				: TEXT("Blueprint asset not found."));
		}
		if (AssetData.IsRedirector() || !AssetData.IsInstanceOf(UBlueprint::StaticClass(), EResolveClass::Yes))
		{
			return FMCPToolResult::Failure(TEXT("The asset is not a Blueprint. Pass the Blueprint asset path, not its generated class or a redirector."));
		}
		UBlueprint* Blueprint = Cast<UBlueprint>(AssetData.GetAsset());
		if (!Blueprint || Blueprint->GetPathName() != Path)
		{
			return FMCPToolResult::Failure(TEXT("The Blueprint could not be loaded at the requested path."));
		}

		const bool bIncludeInherited = BoolArg(Arguments, TEXT("include_inherited"), false);
		const UClass* GeneratedClass = Blueprint->GeneratedClass;
		const UObject* ClassDefaults = GeneratedClass ? GeneratedClass->GetDefaultObject(false) : nullptr;
		TArray<const UBlueprint*> Sources{Blueprint};
		if (bIncludeInherited)
		{
			for (const UClass* ParentClass = GeneratedClass ? GeneratedClass->GetSuperClass() : Blueprint->ParentClass.Get();
				ParentClass; ParentClass = ParentClass->GetSuperClass())
			{
				if (const UBlueprint* Parent = Cast<UBlueprint>(ParentClass->ClassGeneratedBy))
				{
					Sources.AddUnique(Parent);
				}
			}
		}

		struct FVariableEntry
		{
			const UBlueprint* Source;
			const FBPVariableDescription* Description;
		};
		TArray<FVariableEntry> AllVariables;
		for (const UBlueprint* Source : Sources)
		{
			for (const FBPVariableDescription& Variable : Source->NewVariables)
			{
				AllVariables.Add({Source, &Variable});
			}
		}

		const int32 Offset = IntArg(Arguments, TEXT("offset"), 0);
		const int32 Limit = IntArg(Arguments, TEXT("limit"), 100);
		const int32 End = static_cast<int32>(FMath::Min<int64>(AllVariables.Num(), static_cast<int64>(Offset) + Limit));
		constexpr int32 MaxDefaultChars = 4096;
		TArray<TSharedPtr<FJsonValue>> Variables;
		for (int32 Index = Offset; Index < End; ++Index)
		{
			const FVariableEntry& Entry = AllVariables[Index];
			const FBPVariableDescription& Variable = *Entry.Description;
			auto Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Variable.VarName.ToString());
			Item->SetObjectField(TEXT("type"), BlueprintVariableType(Variable));
			Item->SetStringField(TEXT("declared_in"), Entry.Source->GetPathName());
			Item->SetBoolField(TEXT("inherited"), Entry.Source != Blueprint);
			if (!Variable.FriendlyName.IsEmpty())
			{
				Item->SetStringField(TEXT("friendly_name"), Variable.FriendlyName);
			}
			if (!Variable.Category.IsEmpty())
			{
				Item->SetStringField(TEXT("category"), Variable.Category.ToString());
			}
			FString DefaultValue;
			FString DefaultSource = TEXT("unavailable");
			if (ClassDefaults && GeneratedClass)
			{
				if (const FProperty* Property = GeneratedClass->FindPropertyByName(Variable.VarName);
					Property && Property->GetOwnerClass() == Entry.Source->GeneratedClass)
				{
					const void* Value = Property->ContainerPtrToValuePtr<void>(ClassDefaults);
					Property->ExportTextItem_Direct(DefaultValue, Value, nullptr, const_cast<UObject*>(ClassDefaults), PPF_None);
					DefaultSource = TEXT("generated_class_cdo");
					Item->SetStringField(TEXT("property_type"), Property->GetCPPType());
				}
			}
			if (DefaultSource == TEXT("unavailable") && !Variable.DefaultValue.IsEmpty())
			{
				DefaultValue = Variable.DefaultValue;
				DefaultSource = TEXT("blueprint_descriptor");
			}
			Item->SetStringField(TEXT("default_source"), DefaultSource);
			if (DefaultSource != TEXT("unavailable"))
			{
				const bool bTruncated = DefaultValue.Len() > MaxDefaultChars;
				Item->SetStringField(TEXT("default_value"), bTruncated ? DefaultValue.Left(MaxDefaultChars) : DefaultValue);
				Item->SetBoolField(TEXT("default_truncated"), bTruncated);
			}
			Variables.Add(MakeShared<FJsonValueObject>(Item));
		}

		auto Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("read_at_utc"), FDateTime::UtcNow().ToIso8601());
		Data->SetStringField(TEXT("object_path"), Path);
		Data->SetBoolField(TEXT("include_inherited"), bIncludeInherited);
		Data->SetBoolField(TEXT("has_generated_class"), GeneratedClass != nullptr);
		Data->SetBoolField(TEXT("has_class_defaults"), ClassDefaults != nullptr);
		Data->SetBoolField(TEXT("generated_class_may_be_stale"), Blueprint->Status != BS_UpToDate && Blueprint->Status != BS_UpToDateWithWarnings);
		Data->SetNumberField(TEXT("total"), AllVariables.Num());
		Data->SetNumberField(TEXT("offset"), Offset);
		Data->SetNumberField(TEXT("limit"), Limit);
		Data->SetNumberField(TEXT("max_default_chars"), MaxDefaultChars);
		int32 ReturnedEnd = End;
		bool bSizeLimited = false;
		auto RefreshPage = [&]()
		{
			Data->SetArrayField(TEXT("variables"), Variables);
			Data->SetBoolField(TEXT("hasMore"), ReturnedEnd < AllVariables.Num());
			Data->SetBoolField(TEXT("response_size_limited"), bSizeLimited);
			if (ReturnedEnd < AllVariables.Num())
			{
				Data->SetNumberField(TEXT("nextOffset"), ReturnedEnd);
			}
			else
			{
				Data->RemoveField(TEXT("nextOffset"));
			}
		};
		RefreshPage();
		// The Workbench serializes both MCP text content and structured content into
		// its model context. Size the complete MCP result below its 16,000-char cap
		// so the pagination cursor is not lost to a downstream preview truncation.
		constexpr int32 MaxSerializedResultChars = 14000;
		for (;;)
		{
			FMCPToolResult Result = DataResult(Data);
			FString SerializedResult;
			FJsonSerializer::Serialize(Result.ToJson(), TJsonWriterFactory<>::Create(&SerializedResult));
			if (SerializedResult.Len() <= MaxSerializedResultChars)
			{
				return Result;
			}
			if (Variables.Num() > 1)
			{
				Variables.Pop();
				--ReturnedEnd;
				bSizeLimited = true;
				RefreshPage();
				continue;
			}
			if (!Variables.IsEmpty())
			{
				const TSharedPtr<FJsonObject> Item = Variables[0]->AsObject();
				FString Value;
				if (Item->TryGetStringField(TEXT("default_value"), Value) && !Value.IsEmpty())
				{
					Item->SetStringField(TEXT("default_value"), Value.Left(Value.Len() / 2));
					Item->SetBoolField(TEXT("default_truncated"), true);
					bSizeLimited = true;
					RefreshPage();
					continue;
				}
				bool bRemovedOptional = false;
				for (const TCHAR* Optional : {TEXT("friendly_name"), TEXT("category"), TEXT("property_type")})
				{
					if (Item->HasField(Optional))
					{
						Item->RemoveField(Optional);
						bRemovedOptional = true;
						break;
					}
				}
				if (bRemovedOptional)
				{
					bSizeLimited = true;
					RefreshPage();
					continue;
				}
			}
			return FMCPToolResult::Failure(TEXT("Blueprint variable metadata exceeds the tool response size limit."));
		}
	}

	FMCPToolResult OpenAsset(const FMCPToolArguments& Arguments)
	{
		FString Path = FPackageName::ExportTextPathToObjectPath(StringArg(Arguments, TEXT("object_path")));
		if (!Path.Contains(TEXT(".")) && FPackageName::IsValidLongPackageName(Path, true))
		{
			Path += TEXT(".") + FPackageName::GetLongPackageAssetName(Path);
		}
		if (!FPackageName::IsValidObjectPath(Path) || !Path.Contains(TEXT(".")) || Path.Contains(TEXT(":")))
		{
			return FMCPToolResult::ProtocolError(-32602, TEXT("Provide an asset object path, package path or copied asset reference, e.g. /Game/Folder/Asset.Asset."));
		}
		if (!GEditor || IsRunningCommandlet())
		{
			return FMCPToolResult::Failure(TEXT("Opening assets requires a running Unreal Editor session."));
		}
		const FAssetData AssetData = Assets().GetAssetByObjectPath(FSoftObjectPath(Path));
		if (!AssetData.IsValid())
		{
			return FMCPToolResult::Failure(Assets().IsLoadingAssets()
				? TEXT("Asset not found while the Asset Registry is scanning; retry after scanning completes.")
				: TEXT("Asset not found. Use assets.search to obtain its objectPath."));
		}
		UObject* Asset = AssetData.GetAsset();
		UAssetEditorSubsystem* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (!Asset || !Editors)
		{
			return FMCPToolResult::Failure(TEXT("Could not load the asset or access the asset editor subsystem."));
		}
		const FString ResolvedPath = Asset->GetPathName();
		const bool bIsWorld = Asset->IsA<UWorld>();
		const bool bAlreadyOpen = bIsWorld
			? GEditor->GetEditorWorldContext().World() == Asset
			: Editors->FindEditorForAsset(Asset, false) != nullptr;
		FText Error;
		if (!Editors->CanOpenEditorForAsset(Asset, EAssetTypeActivationOpenedMethod::Edit, &Error))
		{
			return FMCPToolResult::Failure(Error.IsEmpty() ? TEXT("This asset cannot be opened for editing.") : Error.ToString());
		}
		if (!Editors->OpenEditorForAsset(Asset, EToolkitMode::Standalone, {}, false))
		{
			return FMCPToolResult::Failure(TEXT("Unreal Editor could not open the asset."));
		}
		// Opening a level (or some specialized editors) can recreate its UObject.
		UObject* OpenedAsset = FSoftObjectPath(ResolvedPath).ResolveObject();
		const bool bOpened = OpenedAsset && (bIsWorld
			? GEditor->GetEditorWorldContext().World() == OpenedAsset
			: Editors->FindEditorForAsset(OpenedAsset, false) != nullptr);
		if (!bOpened)
		{
			return FMCPToolResult::Failure(TEXT("The open request returned, but no open editor could be confirmed. It may have been cancelled or deferred by this asset type."));
		}
		auto Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("objectPath"), ResolvedPath);
		Data->SetBoolField(TEXT("opened"), true);
		Data->SetBoolField(TEXT("alreadyOpen"), bAlreadyOpen);
		Data->SetBoolField(TEXT("isLevel"), bIsWorld);
		return DataResult(Data);
	}

	FMCPToolResult ListActors(const FMCPToolArguments& Arguments)
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			return FMCPToolResult::Failure(TEXT("No editor world is open."));
		}
		const bool bSelectedOnly = BoolArg(Arguments, TEXT("selected_only"), false);
		const FString Name = StringArg(Arguments, TEXT("name_contains"));
		TArray<AActor*> Found;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if ((!bSelectedOnly || GEditor->GetSelectedActors()->IsSelected(*It)) && It->GetActorLabel().Contains(Name))
			{
				Found.Add(*It);
			}
		}
		Found.Sort([](const AActor& A, const AActor& B) { return A.GetPathName() < B.GetPathName(); });
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const AActor* Actor : Found)
		{
			auto Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("objectPath"), Actor->GetPathName());
			Item->SetStringField(TEXT("label"), Actor->GetActorLabel());
			Item->SetStringField(TEXT("classPath"), Actor->GetClass()->GetPathName());
			Item->SetBoolField(TEXT("selected"), GEditor->GetSelectedActors()->IsSelected(Actor));
			auto Location = MakeShared<FJsonObject>();
			const FVector Position = Actor->GetActorLocation();
			Location->SetNumberField(TEXT("x"), Position.X);
			Location->SetNumberField(TEXT("y"), Position.Y);
			Location->SetNumberField(TEXT("z"), Position.Z);
			Item->SetObjectField(TEXT("location"), Location);
			Items.Add(MakeShared<FJsonValueObject>(Item));
		}
		auto Data = Page(Items, IntArg(Arguments, TEXT("offset"), 0), IntArg(Arguments, TEXT("limit"), 100));
		Data->SetStringField(TEXT("world"), World->GetPathName());
		return DataResult(Data);
	}

	bool IsSourceFile(const FString& Path)
	{
		static const TSet<FString> Extensions{TEXT("h"), TEXT("hpp"), TEXT("cpp"), TEXT("c"), TEXT("cc"),
			TEXT("cs"), TEXT("inl"), TEXT("py"), TEXT("usf"), TEXT("ush"), TEXT("json"), TEXT("md"), TEXT("txt")};
		return Extensions.Contains(FPaths::GetExtension(Path).ToLower());
	}

	bool ResolveSourcePath(const FString& Root, const FString& Relative, FString& Absolute)
	{
		FString Clean = Relative;
		Clean.ReplaceInline(TEXT("\\"), TEXT("/"));
		if ((!Clean.IsEmpty() && !FPaths::IsRelative(Clean)) || Clean.Contains(TEXT(":")))
		{
			return false;
		}
		TArray<FString> Parts;
		Clean.ParseIntoArray(Parts, TEXT("/"));
		Absolute = Root;
		auto& Platform = FPlatformFileManager::Get().GetPlatformFile();
		if (Platform.IsSymlink(*Root) != ESymlinkResult::NonSymlink)
		{
			return false;
		}
		for (const FString& Part : Parts)
		{
			if (Part.EndsWith(TEXT(".")) || Part.EndsWith(TEXT(" ")))
			{
				return false;
			}
			Absolute /= Part;
			if (Platform.IsSymlink(*Absolute) != ESymlinkResult::NonSymlink)
			{
				return false;
			}
		}
		return true;
	}

	FMCPToolResult ListSource(const FString& Root, const FString& Relative, bool bRecursive, int32 Offset, int32 Limit)
	{
		FString Absolute;
		if (!ResolveSourcePath(Root, Relative, Absolute))
		{
			return FMCPToolResult::ProtocolError(-32602, TEXT("Use a relative source path without dot segments, absolute paths or links."));
		}
		auto& Platform = FPlatformFileManager::Get().GetPlatformFile();
		if (!Platform.DirectoryExists(*Absolute))
		{
			return FMCPToolResult::Failure(TEXT("Source directory does not exist."));
		}
		TArray<FString> Directories{Absolute};
		TArray<FString> Files;
		for (int32 Index = 0; Index < Directories.Num(); ++Index)
		{
			const FString Directory = Directories[Index];
			if (!Platform.IterateDirectory(*Directory, [&](const TCHAR* Path, bool bDirectory)
			{
				if (Platform.IsSymlink(Path) != ESymlinkResult::NonSymlink)
				{
					return true;
				}
				if (bDirectory && bRecursive)
				{
					Directories.Add(Path);
				}
				else if (!bDirectory && IsSourceFile(Path))
				{
					FString File = Path;
					FPaths::MakePathRelativeTo(File, *(Root + TEXT("/")));
					Files.Add(MoveTemp(File));
				}
				return true;
			}))
			{
				return FMCPToolResult::Failure(TEXT("Unable to enumerate source directory."));
			}
		}
		Files.Sort();
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FString& File : Files)
		{
			Items.Add(MakeShared<FJsonValueString>(File));
		}
		return DataResult(Page(Items, Offset, Limit));
	}

	FMCPToolResult ReadSource(const FString& Root, const FString& Relative, int32 StartLine, int32 MaxLines)
	{
		FString Absolute;
		if (Relative.IsEmpty() || !ResolveSourcePath(Root, Relative, Absolute) || !IsSourceFile(Absolute))
		{
			return FMCPToolResult::ProtocolError(-32602, TEXT("path must identify a supported text file under the source root, without links or dot segments."));
		}
		// Read using one handle with a fixed upper bound, including if the file changes size.
		auto& Platform = FPlatformFileManager::Get().GetPlatformFile();
		TUniquePtr<IFileHandle> File(Platform.OpenRead(*Absolute));
		if (!File)
		{
			return FMCPToolResult::Failure(TEXT("Source file could not be opened."));
		}
		const int64 Size = File->Size();
		if (Size < 0 || Size > 1024 * 1024)
		{
			return FMCPToolResult::Failure(TEXT("Source file exceeds the 1 MiB read limit."));
		}
		TArray<uint8> Bytes;
		Bytes.SetNumUninitialized(Size + 2);
		if (Size > 0 && !File->Read(Bytes.GetData(), Size))
		{
			return FMCPToolResult::Failure(TEXT("Failed to read source file."));
		}
		Bytes[Size] = 0;
		Bytes[Size + 1] = 0;
		FString Text;
		FFileHelper::BufferToString(Text, Bytes.GetData(), Size);
		Text.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
		Text.ReplaceInline(TEXT("\r"), TEXT("\n"));
		TArray<FString> Lines;
		Text.ParseIntoArray(Lines, TEXT("\n"), false);
		if (Text.EndsWith(TEXT("\n")) && !Lines.IsEmpty() && Lines.Last().IsEmpty())
		{
			Lines.Pop();
		}
		const int32 Begin = StartLine - 1;
		const int32 End = static_cast<int32>(FMath::Min<int64>(Lines.Num(), static_cast<int64>(Begin) + MaxLines));
		TArray<TSharedPtr<FJsonValue>> Values;
		for (int32 Index = Begin; Index < End; ++Index)
		{
			Values.Add(MakeShared<FJsonValueString>(Lines[Index]));
		}
		auto Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("path"), Relative);
		Data->SetNumberField(TEXT("startLine"), StartLine);
		Data->SetNumberField(TEXT("totalLines"), Lines.Num());
		Data->SetArrayField(TEXT("lines"), Values);
		Data->SetBoolField(TEXT("hasMore"), End < Lines.Num());
		if (End < Lines.Num())
		{
			Data->SetNumberField(TEXT("nextLine"), End + 1);
		}
		return DataResult(Data);
	}
}

FString ValidateMCPToolArguments(const FMCPToolArguments& Arguments, const TSharedRef<FJsonObject>& Schema, bool bCheckRequired)
{
	return Validate(Arguments, Schema, bCheckRequired);
}

TArray<FMCPToolBinding> CreateMCPNativeHandlers(TArray<TFuture<void>>& BackgroundTasks)
{
	TArray<FMCPToolBinding> NativeHandlers;
	auto Register = [&](FMCPToolDefinition Definition, FMCPSyncToolHandler Handler)
	{
		FMCPToolBinding Binding;
		Binding.Definition = Definition;
		Binding.Handler = [Schema = Definition.InputSchema.ToSharedRef(), Handler](const FMCPToolArguments& Arguments, FMCPToolCompletion Complete)
		{
			const FString Problem = Validate(Arguments, Schema);
			Complete(Problem.IsEmpty() ? Handler(Arguments) : FMCPToolResult::ProtocolError(-32602, Problem));
		};
		NativeHandlers.Add(MoveTemp(Binding));
	};

	Register(Define(TEXT("editor_info"), TEXT("Return the current Unreal Engine version."), MakeShared<FJsonObject>()),
		[](const FMCPToolArguments&)
		{
			auto Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("engineVersion"), FEngineVersion::Current().ToString());
			Data->SetStringField(TEXT("module"), TEXT("BSHarnessTools"));
			return DataResult(Data);
		});

	Register(Define(TEXT("project_info"), TEXT("Get current project name, project paths and plugin source root."), MakeShared<FJsonObject>()),
		[](const FMCPToolArguments&)
		{
			auto Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("projectName"), FApp::GetProjectName());
			Data->SetStringField(TEXT("projectFile"), FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()));
			Data->SetStringField(TEXT("projectSource"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Source")));
			const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("BSHarness"));
			if (Plugin)
			{
				Data->SetStringField(TEXT("pluginSource"), FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Source")));
			}
			return DataResult(Data);
		});

	auto Properties = MakeShared<FJsonObject>();
	AddString(Properties, TEXT("path"), TEXT("Package folder; default /Game."));
	AddString(Properties, TEXT("class_path"), TEXT("Exact asset class path, e.g. /Script/Engine.Material; omitted means all classes."));
	AddString(Properties, TEXT("name_contains"), TEXT("Case-insensitive asset name substring."));
	AddBool(Properties, TEXT("recursive"), TEXT("Include subfolders."), true);
	AddPaging(Properties);
	Register(Define(TEXT("assets.search"), TEXT("Search asset registry metadata without loading assets. Check isScanning before assuming complete results."), Properties), SearchAssets);

	Properties = MakeShared<FJsonObject>();
	AddString(Properties, TEXT("object_path"), TEXT("Full object path returned by assets.search."));
	Register(Define(TEXT("assets.get"), TEXT("Get asset metadata and package dependencies without loading the asset."), Properties, {TEXT("object_path")}), GetAsset);

	Properties = MakeShared<FJsonObject>();
	AddString(Properties, TEXT("object_path"), TEXT("Canonical /Game Blueprint object path, e.g. /Game/Folder/BP_Name.BP_Name."));
	AddBool(Properties, TEXT("include_inherited"), TEXT("Include variables declared by parent Blueprints; excludes native C++ properties."), false);
	AddInteger(Properties, TEXT("offset"), 0, 0, MAX_int32);
	AddInteger(Properties, TEXT("limit"), 100, 1, 100);
	Register(Define(TEXT("blueprint.variables"), TEXT("Read Blueprint member variables and their class default values where available, without compiling or saving. Values are text, capped at 4096 characters each; use offset and limit to page variables."), Properties, {TEXT("object_path")}), GetBlueprintVariables);

	Properties = MakeShared<FJsonObject>();
	AddString(Properties, TEXT("object_path"), TEXT("Asset object path, package path or Copy Reference text."));
	Register(Define(TEXT("assets.open"), TEXT("Load and open an asset in its Unreal Editor, or focus an existing editor. Opening a map changes the editor world and may prompt to save."), Properties, {TEXT("object_path")}), OpenAsset);

	Properties = MakeShared<FJsonObject>();
	AddBool(Properties, TEXT("selected_only"), TEXT("Return only selected editor actors."), false);
	AddString(Properties, TEXT("name_contains"), TEXT("Case-insensitive actor label substring."));
	AddPaging(Properties);
	Register(Define(TEXT("actors.list"), TEXT("List loaded actors in the editor world, including labels, classes and locations. Excludes PIE and unloaded World Partition actors."), Properties), ListActors);

	const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("BSHarness"));
	const FString PluginRoot = Plugin ? FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Source")) : FString();
	const FString ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Source"));
	for (bool bRead : {false, true})
	{
		Properties = MakeShared<FJsonObject>();
		auto RootRule = Property(TEXT("string"), TEXT("Source root: plugin (default) or project."));
		RootRule->SetArrayField(TEXT("enum"), {MakeShared<FJsonValueString>(TEXT("plugin")), MakeShared<FJsonValueString>(TEXT("project"))});
		RootRule->SetStringField(TEXT("default"), TEXT("plugin"));
		Properties->SetObjectField(TEXT("root"), RootRule);
		AddString(Properties, TEXT("path"), bRead ? TEXT("Relative source file path.") : TEXT("Relative source directory; default is the source root."));
		if (bRead)
		{
			AddInteger(Properties, TEXT("start_line"), 1, 1, MAX_int32);
			AddInteger(Properties, TEXT("max_lines"), 200, 1, 1000);
		}
		else
		{
			AddBool(Properties, TEXT("recursive"), TEXT("Include subdirectories; links are skipped."), true);
			AddPaging(Properties);
		}
		auto Definition = Define(bRead ? TEXT("source.read") : TEXT("source.list"),
			bRead ? TEXT("Read lines from a source text file asynchronously; at most 1 MiB per file.")
				: TEXT("List source text files asynchronously under project Source or BSHarness Source."),
			Properties, bRead ? std::initializer_list<const TCHAR*>{TEXT("path")} : std::initializer_list<const TCHAR*>{});
		FMCPToolBinding Binding;
		Binding.Definition = Definition;
		Binding.Handler =
			[&BackgroundTasks, Schema = Definition.InputSchema.ToSharedRef(), bRead, PluginRoot, ProjectRoot](const FMCPToolArguments& Arguments, FMCPToolCompletion Complete)
			{
				const FString Problem = Validate(Arguments, Schema);
				if (!Problem.IsEmpty())
				{
					Complete(FMCPToolResult::ProtocolError(-32602, Problem));
					return;
				}
				const FString Root = StringArg(Arguments, TEXT("root"), TEXT("plugin")) == TEXT("plugin") ? PluginRoot : ProjectRoot;
				if (Root.IsEmpty())
				{
					Complete(FMCPToolResult::Failure(TEXT("Source root is unavailable.")));
					return;
				}
				const FString Path = StringArg(Arguments, TEXT("path"));
				const int32 First = IntArg(Arguments, bRead ? TEXT("start_line") : TEXT("offset"), bRead ? 1 : 0);
				const int32 Count = IntArg(Arguments, bRead ? TEXT("max_lines") : TEXT("limit"), bRead ? 200 : 100);
				const bool bRecursive = BoolArg(Arguments, TEXT("recursive"), true);
				BackgroundTasks.RemoveAll([](const TFuture<void>& Task) { return Task.IsReady(); });
				BackgroundTasks.Add(Async(EAsyncExecution::ThreadPool,
					[Root, Path, First, Count, bRead, bRecursive, Complete = MoveTemp(Complete)]() mutable
					{
						Complete(bRead ? ReadSource(Root, Path, First, Count) : ListSource(Root, Path, bRecursive, First, Count));
					}));
			};
		NativeHandlers.Add(MoveTemp(Binding));
	}
	return NativeHandlers;
}
