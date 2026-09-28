#include "MCPToolConfiguration.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "BSHarnessToolsModule.h"
#include "Misc/AutomationTest.h"
#include "Misc/StringOutputDevice.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	FMCPToolBinding EchoHandler()
	{
		FMCPToolBinding Binding;
		Binding.Definition.Name = TEXT("echo");
		Binding.Definition.Description = TEXT("Test native handler");
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(TEXT(R"({
            "type":"object","additionalProperties":false,"required":["message"],
            "properties":{
                "message":{"type":"string"},
                "count":{"type":"integer","minimum":1,"maximum":10,"default":2},
                "mode":{"type":"string","enum":["a","b"],"default":"a"}
            }
        })"))), Binding.Definition.InputSchema);
		Binding.Handler = [](const FMCPToolArguments& Arguments, FMCPToolCompletion Complete)
		{
			Complete(FMCPToolResult::Success(TEXT("echo"), Arguments));
		};
		return Binding;
	}

	const TCHAR* InitialConfig = TEXT(R"({"version":1,"tools":[{
        "name":"configured.echo","handler":"echo","description":"Configured description",
        "parameters":{"message":{"default":"hello"},"count":{"default":3,"maximum":5}}
    }]})");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPConfigDefaultsTest, "BSHarness.MCP.Configuration.DefaultsAndReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPConfigDefaultsTest::RunTest(const FString& Parameters)
{
	FMCPToolRegistry Registry;
	FMCPToolConfiguration Configuration;
	const TArray<FMCPToolBinding> Handlers{EchoHandler()};
	FString Error;
	TestTrue(TEXT("Load configuration"), Configuration.LoadJson(InitialConfig, Handlers, Registry, Error));
	TestEqual(TEXT("One configured tool"), Configuration.NumTools(), 1);
	auto Future = Registry.CallTool(TEXT("configured.echo"), MakeShared<FJsonObject>());
	TestTrue(TEXT("Sync handler still completes synchronously"), Future.IsReady());
	const auto Result = Future.Get();
	TestFalse(TEXT("Configured defaults satisfy required parameters"), Result.bIsError);
	if (Result.StructuredContent)
	{
		TestEqual(TEXT("String default injected"), Result.StructuredContent->GetStringField(TEXT("message")), FString(TEXT("hello")));
		TestEqual(TEXT("Integer default injected"), Result.StructuredContent->GetIntegerField(TEXT("count")), 3);
		TestEqual(TEXT("Inherited default injected"), Result.StructuredContent->GetStringField(TEXT("mode")), FString(TEXT("a")));
	}
	const auto Listed = Registry.ListTools();
	if (!Listed.IsEmpty())
	{
		TestEqual(TEXT("Description is configured"), Listed[0].Description, FString(TEXT("Configured description")));
		TestTrue(TEXT("Defaulted parameter no longer required from caller"), Listed[0].InputSchema->GetArrayField(TEXT("required")).IsEmpty());
		TestEqual(TEXT("Published schema contains narrowed bound"), Listed[0].InputSchema->GetObjectField(TEXT("properties"))->GetObjectField(TEXT("count"))->GetIntegerField(TEXT("maximum")), 5);
	}
	auto Arguments = MakeShared<FJsonObject>();
	Arguments->SetNumberField(TEXT("count"), 4);
	const auto Explicit = Registry.CallTool(TEXT("configured.echo"), Arguments).Get();
	if (Explicit.StructuredContent)
	{
		TestEqual(TEXT("Caller overrides default"), Explicit.StructuredContent->GetIntegerField(TEXT("count")), 4);
	}
	TestFalse(TEXT("Call does not mutate caller arguments"), Arguments->HasField(TEXT("message")));
	Arguments->SetNumberField(TEXT("count"), 6);
	TestEqual(TEXT("Configured bound enforced"), Registry.CallTool(TEXT("configured.echo"), Arguments).Get().ProtocolErrorCode.Get(0), -32602);
	TestEqual(TEXT("Native schema unchanged"), Handlers[0].Definition.InputSchema->GetObjectField(TEXT("properties"))->GetObjectField(TEXT("count"))->GetIntegerField(TEXT("maximum")), 10);
	TestTrue(TEXT("Alias can be renamed and required defaults changed"), Configuration.LoadJson(
		TEXT(R"({"version":1,"tools":[{"name":"renamed.echo","handler":"echo","parameters":{"message":{"default":"changed"}}}]})"), Handlers, Registry, Error));
	TestTrue(TEXT("Old name removed"), Registry.CallTool(TEXT("configured.echo"), MakeShared<FJsonObject>()).Get().bIsError);
	const auto Changed = Registry.CallTool(TEXT("renamed.echo"), MakeShared<FJsonObject>()).Get();
	if (Changed.StructuredContent)
	{
		TestEqual(TEXT("Reload uses new defaults"), Changed.StructuredContent->GetStringField(TEXT("message")), FString(TEXT("changed")));
	}
	TestTrue(TEXT("Tool can be disabled"), Configuration.LoadJson(
		TEXT(R"({"version":1,"tools":[{"name":"renamed.echo","handler":"echo","enabled":false}]})"), Handlers, Registry, Error));
	TestTrue(TEXT("Disabled tool not listed"), Registry.ListTools().IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPConfigRollbackTest, "BSHarness.MCP.Configuration.InvalidReloadRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPConfigRollbackTest::RunTest(const FString& Parameters)
{
	FMCPToolRegistry Registry;
	FMCPToolConfiguration Configuration;
	const TArray<FMCPToolBinding> Handlers{EchoHandler()};
	FString Error;
	TestTrue(TEXT("Load initial working configuration"), Configuration.LoadJson(InitialConfig, Handlers, Registry, Error));
	auto External = EchoHandler();
	External.Definition.Name = TEXT("external.tool");
	Registry.RegisterAsyncTool(External.Definition, External.Handler, Error);
	const TArray<FString> InvalidConfigurations{
		TEXT("{bad}"),
		TEXT(R"({"version":2,"tools":[]})"),
		TEXT(R"({"version":1,"tools":[{"name":"good.name","handler":"echo"},{"name":"bad.name","handler":"unknown"}]})"),
		TEXT(R"({"version":1,"tools":[{"name":"same","handler":"echo"},{"name":"same","handler":"echo","enabled":false}]})"),
		TEXT(R"({"version":1,"tools":[{"name":"bad name","handler":"echo"}]})"),
		TEXT(R"({"version":1,"tools":[{"name":"alias","handler":"echo","enabled":"false"}]})"),
		TEXT(R"({"version":1,"tools":[{"name":"alias","handler":"echo","parameter":{}}]})"),
		TEXT(R"({"version":1,"tools":[{"name":"alias","handler":"echo","parameters":{"missing":{}}}]})"),
		TEXT(R"({"version":1,"tools":[{"name":"alias","handler":"echo","parameters":{"count":{"type":"string"}}}]})"),
		TEXT(R"({"version":1,"tools":[{"name":"alias","handler":"echo","parameters":{"count":{"default":"bad"}}}]})"),
		TEXT(R"({"version":1,"tools":[{"name":"alias","handler":"echo","parameters":{"count":{"minimum":0}}}]})"),
		TEXT(R"({"version":1,"tools":[{"name":"alias","handler":"echo","parameters":{"count":{"maximum":11}}}]})"),
		TEXT(R"({"version":1,"tools":[{"name":"alias","handler":"echo","parameters":{"count":{"maximum":1}}}]})"),
		TEXT(R"({"version":1,"tools":[{"name":"alias","handler":"echo","parameters":{"mode":{"enum":["c"]}}}]})"),
		TEXT(R"({"version":1,"tools":[{"name":"external.tool","handler":"echo"}]})")
	};
	for (int32 Index = 0; Index < InvalidConfigurations.Num(); ++Index)
	{
		TestFalse(FString::Printf(TEXT("Reject invalid configuration %d"), Index), Configuration.LoadJson(InvalidConfigurations[Index], Handlers, Registry, Error));
		TestFalse(TEXT("Error explains rejection"), Error.IsEmpty());
		TestEqual(TEXT("No partial registration"), Registry.ListTools().Num(), 2);
		TestFalse(TEXT("Working tool survives failed reload"), Registry.CallTool(TEXT("configured.echo"), MakeShared<FJsonObject>()).Get().bIsError);
	}
	TestFalse(TEXT("Missing file preserves registry"), Configuration.LoadFile(TEXT("__missing_MCPTools_file__.json"), Handlers, Registry, Error));
	TestTrue(TEXT("Empty configuration clears managed tools"), Configuration.LoadJson(TEXT(R"({"version":1,"tools":[]})"), Handlers, Registry, Error));
	TestEqual(TEXT("Unrelated registration preserved"), Registry.ListTools().Num(), 1);
	TestTrue(TEXT("Remaining tool is external"), Registry.ListTools()[0].Name.Equals(TEXT("external.tool"), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPConfigPendingTest, "BSHarness.MCP.Configuration.PendingCallsSurvive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPConfigPendingTest::RunTest(const FString& Parameters)
{
	FMCPToolRegistry Registry;
	FMCPToolConfiguration Configuration;
	auto Native = EchoHandler();
	TArray<FMCPToolCompletion> Completions;
	TArray<FString> Messages;
	Native.Handler = [&](const FMCPToolArguments& Arguments, FMCPToolCompletion Complete)
	{
		Messages.Add(Arguments->GetStringField(TEXT("message")));
		Completions.Add(MoveTemp(Complete));
	};
	const TArray<FMCPToolBinding> Handlers{Native};
	FString Error;
	Configuration.LoadJson(InitialConfig, Handlers, Registry, Error);
	auto Before = Registry.CallTool(TEXT("configured.echo"), MakeShared<FJsonObject>());
	TestFalse(TEXT("Async invocation pending"), Before.IsReady());
	TestTrue(TEXT("Reload while invocation pending"), Configuration.LoadJson(
		TEXT(R"({"version":1,"tools":[{"name":"configured.echo","handler":"echo","parameters":{"message":{"default":"after"}}}]})"), Handlers, Registry, Error));
	auto After = Registry.CallTool(TEXT("configured.echo"), MakeShared<FJsonObject>());
	TestEqual(TEXT("Both handlers started"), Messages.Num(), 2);
	if (Messages.Num() == 2)
	{
		TestEqual(TEXT("First invocation keeps original arguments"), Messages[0], FString(TEXT("hello")));
		TestEqual(TEXT("Second invocation uses new arguments"), Messages[1], FString(TEXT("after")));
	}
	Configuration.LoadJson(TEXT(R"({"version":1,"tools":[]})"), Handlers, Registry, Error);
	for (const auto& Complete : Completions)
	{
		Complete(FMCPToolResult::Success(TEXT("completed")));
	}
	TestTrue(TEXT("Old and new pending calls survive removal"), Before.IsReady() && After.IsReady());
	if (Before.IsReady() && After.IsReady())
	{
		TestFalse(TEXT("Old call succeeds"), Before.Get().bIsError);
		TestFalse(TEXT("New call succeeds"), After.Get().bIsError);
	}
	return true;
}

#endif
