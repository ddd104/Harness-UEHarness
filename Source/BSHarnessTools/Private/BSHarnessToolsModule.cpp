#include "BSHarnessToolsModule.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "MCPCommonTools.h"
#include "MCPToolDispatcher.h"
#include "Misc/OutputDevice.h"
#include "Misc/Parse.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogBSHarnessTools, Log, All);

FBSHarnessToolsModule& FBSHarnessToolsModule::Get()
{
	check(IsInGameThread());
	return FModuleManager::LoadModuleChecked<FBSHarnessToolsModule>(TEXT("BSHarnessTools"));
}

FMCPToolRegistry& FBSHarnessToolsModule::GetToolRegistry()
{
	check(IsInGameThread() && Registry);
	return *Registry;
}

void FBSHarnessToolsModule::StartupModule()
{
	Registry = MakeUnique<FMCPToolRegistry>();
	NativeHandlers = CreateMCPNativeHandlers(BackgroundTasks);
	const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("BSHarness"));
	if (Plugin)
	{
		ConfigurationPath = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Config/MCPTools.json"));
	}
	FString Error;
	if (!ReloadToolConfiguration(Error))
	{
		UE_LOG(LogBSHarnessTools, Error, TEXT("Tool configuration failed: %s"), *Error);
	}
}

bool FBSHarnessToolsModule::ReloadToolConfiguration(FString& OutError)
{
	if (!Registry || ConfigurationPath.IsEmpty())
	{
		OutError = TEXT("Tool registry or configuration path is unavailable.");
		return false;
	}
	if (!Configuration.LoadFile(ConfigurationPath, NativeHandlers, *Registry, OutError))
	{
		return false;
	}
	UE_LOG(LogBSHarnessTools, Display, TEXT("Loaded %d configured tools from %s"), Configuration.NumTools(), *ConfigurationPath);
	return true;
}

void FBSHarnessToolsModule::ShutdownModule()
{
	if (Registry)
	{
		Registry->Shutdown();
	}
	// Built-in workers only read files and complete results; none waits for the game thread.
	for (auto& Task : BackgroundTasks)
	{
		Task.Wait();
	}
	BackgroundTasks.Empty();
	NativeHandlers.Empty();
	Registry.Reset();
}

bool FBSHarnessToolsModule::Exec_Editor(UWorld* World, const TCHAR* Cmd, FOutputDevice& Ar)
{
	if (FParse::Command(&Cmd, TEXT("BSHarness.MCP.Reload")))
	{
		FString Error;
		if (!ReloadToolConfiguration(Error))
		{
			Ar.Logf(TEXT("MCP reload failed; existing tools retained: %s"), *Error);
		}
		else
		{
			Ar.Logf(TEXT("Loaded %d tools from %s"), Configuration.NumTools(), *ConfigurationPath);
		}
		return true;
	}
	if (FParse::Command(&Cmd, TEXT("BSHarness.MCP.Handlers")))
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FMCPToolBinding& Handler : NativeHandlers)
		{
			Values.Add(MakeShared<FJsonValueObject>(Handler.Definition.ToJson()));
		}
		auto Data = MakeShared<FJsonObject>();
		Data->SetArrayField(TEXT("handlers"), Values);
		Data->SetStringField(TEXT("configurationFile"), ConfigurationPath);
		FString Json;
		FJsonSerializer::Serialize(Data, TJsonWriterFactory<>::Create(&Json));
		Ar.Log(*Json);
		return true;
	}
	TSharedPtr<FJsonObject> Request;
	bool bShorthand = false;
	if (FParse::Command(&Cmd, TEXT("BSHarness.MCP.List")))
	{
		Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("method"), TEXT("tools/list"));
		bShorthand = true;
	}
	else if (FParse::Command(&Cmd, TEXT("BSHarness.MCP.Call")))
	{
		const FString ToolName = FParse::Token(Cmd, false);
		FString Json = FString(Cmd).TrimStartAndEnd();
		if (Json.IsEmpty())
		{
			Json = TEXT("{}");
		}
		TSharedPtr<FJsonObject> Arguments;
		if (ToolName.IsEmpty() || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Arguments) || !Arguments)
		{
			Ar.Log(TEXT("Usage: BSHarness.MCP.Call <tool_name> {\"argument\":\"value\"}"));
			return true;
		}
		Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("method"), TEXT("tools/call"));
		auto Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("name"), ToolName);
		Params->SetObjectField(TEXT("arguments"), Arguments);
		Request->SetObjectField(TEXT("params"), Params);
		bShorthand = true;
	}
	else if (FParse::Command(&Cmd, TEXT("BSHarness.MCP.Request")))
	{
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(Cmd)), Request) || !Request)
		{
			Ar.Log(TEXT("Expected a JSON-RPC request object after BSHarness.MCP.Request."));
			return true;
		}
	}
	else
	{
		return false;
	}
	if (!Registry || !IsInGameThread())
	{
		Ar.Log(TEXT("MCP console commands require an active registry on the game thread."));
		return true;
	}
	// Shorthand commands get an ID; full Request commands preserve the supplied envelope.
	if (bShorthand)
	{
		Request->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
		Request->SetStringField(TEXT("id"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	}
	FMCPToolDispatcher::Dispatch(*Registry, Request.ToSharedRef()).Next([](TSharedPtr<FJsonObject> Response)
	{
		if (Response)
		{
			FString Json;
			FJsonSerializer::Serialize(Response.ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json));
			UE_LOG(LogBSHarnessTools, Display, TEXT("MCP response: %s"), *Json);
		}
	});
	return true;
}

IMPLEMENT_MODULE(FBSHarnessToolsModule, BSHarnessTools)
