#include "MCPToolConfiguration.h"

#include "MCPCommonTools.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	TSharedRef<FJsonObject> CopyJson(const TSharedRef<FJsonObject>& Source)
	{
		TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>();
		FJsonObject::Duplicate(TSharedPtr<const FJsonObject>(Source), Copy);
		return Copy.ToSharedRef();
	}

	bool OnlyFields(const TSharedRef<FJsonObject>& Object, std::initializer_list<const TCHAR*> Allowed, FString& Error)
	{
		for (const auto& Pair : Object->Values)
		{
			const FString Key(Pair.Key);
			bool bKnown = false;
			for (const TCHAR* Field : Allowed)
			{
				bKnown |= Key.Equals(Field, ESearchCase::CaseSensitive);
			}
			if (!bKnown)
			{
				Error = FString::Printf(TEXT("Unknown configuration field: %s"), *Key);
				return false;
			}
		}
		return true;
	}

	bool ValidName(const FString& Name)
	{
		if (Name.IsEmpty() || Name.Len() > 128)
		{
			return false;
		}
		for (TCHAR C : Name)
		{
			if (!((C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9') || C == '_' || C == '-' || C == '.'))
			{
				return false;
			}
		}
		return true;
	}

	bool ConfigureParameters(const TSharedRef<FJsonObject>& Parameters, const TSharedRef<FJsonObject>& Schema, FString& Error)
	{
		const auto Properties = Schema->GetObjectField(TEXT("properties"));
		for (const auto& Pair : Parameters->Values)
		{
			const FString Name(Pair.Key);
			const TSharedPtr<FJsonObject>* RulePtr = nullptr;
			if (!Properties->TryGetObjectField(Name, RulePtr) || !Pair.Value.IsValid() || Pair.Value->Type != EJson::Object)
			{
				Error = FString::Printf(TEXT("Parameter '%s' must be an existing handler parameter with an object configuration."), *Name);
				return false;
			}
			const auto Rule = *RulePtr;
			const auto Override = Pair.Value->AsObject();
			if (!OnlyFields(Override.ToSharedRef(), {TEXT("description"), TEXT("default"), TEXT("minimum"), TEXT("maximum"), TEXT("enum")}, Error))
			{
				return false;
			}
			if (Override->HasField(TEXT("description")))
			{
				if (!Override->HasTypedField<EJson::String>(TEXT("description")))
				{
					Error = TEXT("Parameter description must be a string.");
					return false;
				}
				Rule->SetStringField(TEXT("description"), Override->GetStringField(TEXT("description")));
			}
			const FString Type = Rule->GetStringField(TEXT("type"));
			for (const TCHAR* Bound : {TEXT("minimum"), TEXT("maximum")})
			{
				if (!Override->HasField(Bound))
				{
					continue;
				}
				if (Type != TEXT("integer") || !Override->HasTypedField<EJson::Number>(Bound))
				{
					Error = TEXT("Only integer parameters accept numeric minimum/maximum.");
					return false;
				}
				const double Value = Override->GetNumberField(Bound);
				const bool bMinimum = FString(Bound) == TEXT("minimum");
				if (!FMath::IsFinite(Value) || Value != FMath::FloorToDouble(Value)
					|| (bMinimum ? Value < Rule->GetNumberField(Bound) : Value > Rule->GetNumberField(Bound)))
				{
					Error = TEXT("Parameter bounds must be integers and may only narrow the handler's accepted range.");
					return false;
				}
				Rule->SetNumberField(Bound, Value);
			}
			if (Type == TEXT("integer") && Rule->GetNumberField(TEXT("minimum")) > Rule->GetNumberField(TEXT("maximum")))
			{
				Error = TEXT("Parameter minimum exceeds maximum.");
				return false;
			}
			if (Override->HasField(TEXT("enum")))
			{
				const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
				if (Type != TEXT("string") || !Override->TryGetArrayField(TEXT("enum"), Values) || Values->IsEmpty())
				{
					Error = TEXT("enum must be a non-empty string array on a string parameter.");
					return false;
				}
				const TArray<TSharedPtr<FJsonValue>>* NativeValues = nullptr;
				Rule->TryGetArrayField(TEXT("enum"), NativeValues);
				for (const auto& Value : *Values)
				{
					if (!Value.IsValid() || Value->Type != EJson::String
						|| (NativeValues && !NativeValues->ContainsByPredicate([&](const auto& Native)
						{
							return Native->AsString().Equals(Value->AsString(), ESearchCase::CaseSensitive);
						})))
					{
						Error = TEXT("enum values must be strings supported by the handler.");
						return false;
					}
				}
				Rule->SetArrayField(TEXT("enum"), *Values);
			}
			if (Override->HasField(TEXT("default")))
			{
				Rule->SetField(TEXT("default"), FJsonValue::Duplicate(Override->TryGetField(TEXT("default"))));
			}
		}
		return true;
	}
}

bool FMCPToolConfiguration::LoadFile(const FString& Path, const TArray<FMCPToolBinding>& NativeHandlers,
	FMCPToolRegistry& Registry, FString& OutError)
{
	const int64 Size = IFileManager::Get().FileSize(*Path);
	if (Size < 0 || Size > 1024 * 1024)
	{
		OutError = FString::Printf(TEXT("Cannot read configuration (missing or larger than 1 MiB): %s"), *Path);
		return false;
	}
	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *Path))
	{
		OutError = FString::Printf(TEXT("Could not read configuration: %s"), *Path);
		return false;
	}
	return LoadJson(Json, NativeHandlers, Registry, OutError);
}

bool FMCPToolConfiguration::LoadJson(const FString& Json, const TArray<FMCPToolBinding>& NativeHandlers,
	FMCPToolRegistry& Registry, FString& OutError)
{
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("Configuration loading requires the game thread.");
		return false;
	}
	TSharedPtr<FJsonObject> Document;
	const auto Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Document) || !Document)
	{
		OutError = TEXT("Invalid configuration JSON: ") + Reader->GetErrorMessage();
		return false;
	}
	if (!OnlyFields(Document.ToSharedRef(), {TEXT("version"), TEXT("tools")}, OutError))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
	if (!Document->HasTypedField<EJson::Number>(TEXT("version")) || Document->GetNumberField(TEXT("version")) != 1
		|| !Document->TryGetArrayField(TEXT("tools"), Tools) || Tools->Num() > 1000)
	{
		OutError = TEXT("Expected version: 1 and a tools array with at most 1000 entries.");
		return false;
	}
	TArray<FMCPToolBinding> Bindings;
	TArray<FString> AllNames;
	for (int32 Index = 0; Index < Tools->Num(); ++Index)
	{
		const auto& Value = (*Tools)[Index];
		auto Fail = [&](const FString& Error)
		{
			OutError = FString::Printf(TEXT("tools[%d]: %s"), Index, *Error);
			return false;
		};
		if (!Value.IsValid() || Value->Type != EJson::Object)
		{
			return Fail(TEXT("Tool entry must be an object."));
		}
		const auto Entry = Value->AsObject();
		FString Problem;
		if (!OnlyFields(Entry.ToSharedRef(), {TEXT("name"), TEXT("handler"), TEXT("description"), TEXT("enabled"), TEXT("parameters")}, Problem))
		{
			return Fail(Problem);
		}
		if (!Entry->HasTypedField<EJson::String>(TEXT("name")) || !Entry->HasTypedField<EJson::String>(TEXT("handler")))
		{
			return Fail(TEXT("name and handler must be strings."));
		}
		const FString Name = Entry->GetStringField(TEXT("name"));
		const FString HandlerId = Entry->GetStringField(TEXT("handler"));
		if (!ValidName(Name) || AllNames.ContainsByPredicate([&](const FString& Existing) { return Existing.Equals(Name, ESearchCase::CaseSensitive); }))
		{
			return Fail(TEXT("Invalid or duplicate tool name: ") + Name);
		}
		AllNames.Add(Name);
		const FMCPToolBinding* Native = NativeHandlers.FindByPredicate([&](const FMCPToolBinding& Candidate)
		{
			return Candidate.Definition.Name.Equals(HandlerId, ESearchCase::CaseSensitive);
		});
		if (!Native)
		{
			return Fail(TEXT("Unknown native handler: ") + HandlerId);
		}
		if ((Entry->HasField(TEXT("description")) && !Entry->HasTypedField<EJson::String>(TEXT("description")))
			|| (Entry->HasField(TEXT("enabled")) && !Entry->HasTypedField<EJson::Boolean>(TEXT("enabled"))))
		{
			return Fail(TEXT("description must be a string; enabled must be a boolean."));
		}
		auto Schema = CopyJson(Native->Definition.InputSchema.ToSharedRef());
		if (Entry->HasField(TEXT("parameters")))
		{
			const TSharedPtr<FJsonObject>* Parameters = nullptr;
			if (!Entry->TryGetObjectField(TEXT("parameters"), Parameters))
			{
				return Fail(TEXT("parameters must be an object."));
			}
			if (!ConfigureParameters(Parameters->ToSharedRef(), Schema, Problem))
			{
				return Fail(Problem);
			}
		}
		auto Defaults = MakeShared<FJsonObject>();
		for (const auto& Pair : Schema->GetObjectField(TEXT("properties"))->Values)
		{
			const auto Rule = Pair.Value->AsObject();
			if (Rule->HasField(TEXT("default")))
			{
				Defaults->SetField(Pair.Key, FJsonValue::Duplicate(Rule->TryGetField(TEXT("default"))));
			}
		}
		Problem = ValidateMCPToolArguments(Defaults, Schema, false);
		if (!Problem.IsEmpty())
		{
			return Fail(TEXT("Invalid parameter defaults: ") + Problem);
		}
		TArray<TSharedPtr<FJsonValue>> Required = Schema->GetArrayField(TEXT("required"));
		Required.RemoveAll([&](const auto& Field) { return Defaults->HasField(Field->AsString()); });
		Schema->SetArrayField(TEXT("required"), Required);
		FMCPToolBinding Binding;
		Binding.Definition = Native->Definition;
		Binding.Definition.Name = Name;
		Binding.Definition.InputSchema = Schema;
		Entry->TryGetStringField(TEXT("description"), Binding.Definition.Description);
		Binding.Handler = [Schema, Defaults, Handler = Native->Handler](const FMCPToolArguments& Arguments, FMCPToolCompletion Complete)
		{
			auto EffectiveArguments = CopyJson(Arguments);
			for (const auto& Pair : Defaults->Values)
			{
				if (!EffectiveArguments->HasField(Pair.Key))
				{
					EffectiveArguments->SetField(Pair.Key, FJsonValue::Duplicate(Pair.Value));
				}
			}
			const FString Error = ValidateMCPToolArguments(EffectiveArguments, Schema);
			if (!Error.IsEmpty())
			{
				Complete(FMCPToolResult::ProtocolError(-32602, Error));
				return;
			}
			Handler(EffectiveArguments, MoveTemp(Complete));
		};
		if (!Entry->HasField(TEXT("enabled")) || Entry->GetBoolField(TEXT("enabled")))
		{
			Bindings.Add(MoveTemp(Binding));
		}
	}
	if (!Registry.ReplaceTools(ManagedNames, Bindings, OutError))
	{
		return false;
	}
	ManagedNames.Reset();
	for (const FMCPToolBinding& Binding : Bindings)
	{
		ManagedNames.Add(Binding.Definition.Name);
	}
	return true;
}
