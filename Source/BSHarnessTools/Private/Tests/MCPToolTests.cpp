#include "MCPToolDispatcher.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/Async.h"
#include "BSHarnessToolsModule.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"

namespace
{
	FMCPToolDefinition MakeDefinition(const FString& Name)
	{
		FMCPToolDefinition Definition;
		Definition.Name = Name;
		Definition.Description = TEXT("Automation test tool");
		Definition.InputSchema = MakeShared<FJsonObject>();
		Definition.InputSchema->SetStringField(TEXT("type"), TEXT("object"));
		return Definition;
	}

	TSharedRef<FJsonObject> MakeRequest(const FString& Method)
	{
		auto Request = MakeShared<FJsonObject>();
		Request->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
		Request->SetStringField(TEXT("id"), TEXT("test-request"));
		Request->SetStringField(TEXT("method"), Method);
		return Request;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPRegistryTest, "BSHarness.MCP.Registry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPRegistryTest::RunTest(const FString& Parameters)
{
	FMCPToolRegistry Registry;
	FString Error;
	auto Definition = MakeDefinition(TEXT("test.echo"));
	int32 Invocations = 0;
	auto Handler = [&Invocations](const FMCPToolArguments& Arguments)
	{
		++Invocations;
		return FMCPToolResult::Success(TEXT("done"));
	};
	TestTrue(TEXT("Register sync tool"), Registry.RegisterSyncTool(Definition, Handler, Error));
	TestFalse(TEXT("Duplicate registration rejected"), Registry.RegisterSyncTool(Definition, Handler, Error));
	TestFalse(TEXT("Invalid name rejected"), Registry.RegisterSyncTool(MakeDefinition(TEXT("bad name")), Handler, Error));
	TestFalse(TEXT("Empty handler rejected"), Registry.RegisterSyncTool(MakeDefinition(TEXT("empty")), {}, Error));
	Definition.InputSchema->SetStringField(TEXT("type"), TEXT("string"));
	TestEqual(TEXT("Registration snapshots schema"), Registry.ListTools()[0].InputSchema->GetStringField(TEXT("type")), FString(TEXT("object")));
	auto Listed = Registry.ListTools();
	Listed[0].InputSchema->SetStringField(TEXT("type"), TEXT("array"));
	TestEqual(TEXT("Listing cannot mutate registry schema"), Registry.ListTools()[0].InputSchema->GetStringField(TEXT("type")), FString(TEXT("object")));
	TestFalse(TEXT("Non-object schema rejected"), Registry.RegisterSyncTool(Definition, Handler, Error));

	auto Result = Registry.CallTool(TEXT("test.echo"), MakeShared<FJsonObject>());
	TestTrue(TEXT("Sync call immediately ready"), Result.IsReady());
	TestFalse(TEXT("Sync call succeeds"), Result.Get().bIsError);
	TestEqual(TEXT("Handler called once"), Invocations, 1);
	auto Missing = Registry.CallTool(TEXT("TEST.ECHO"), MakeShared<FJsonObject>());
	TestEqual(TEXT("Names are case sensitive"), Missing.Get().ProtocolErrorCode.Get(-1), -32602);
	auto BadTimeout = Registry.CallTool(TEXT("test.echo"), MakeShared<FJsonObject>(), 0.0);
	TestTrue(TEXT("Invalid timeout rejected"), BadTimeout.Get().bIsError);
	TestEqual(TEXT("Rejected requests have no side effects"), Invocations, 1);
	TestTrue(TEXT("Case-distinct tool can register separately"), Registry.RegisterSyncTool(MakeDefinition(TEXT("TEST.ECHO")), Handler, Error));
	TestFalse(TEXT("Wrong-case unregister cannot remove tool"), Registry.UnregisterTool(TEXT("Test.Echo")));
	TestTrue(TEXT("Upper-case tool removed separately"), Registry.UnregisterTool(TEXT("TEST.ECHO")));
	TestTrue(TEXT("Tool removed"), Registry.UnregisterTool(TEXT("test.echo")));
	TestTrue(TEXT("List is empty"), Registry.ListTools().IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPAsyncTest, "BSHarness.MCP.AsyncCompletion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPAsyncTest::RunTest(const FString& Parameters)
{
	FMCPToolRegistry Registry;
	FString Error;
	FMCPToolCompletion Completion;
	TSharedPtr<FJsonObject> ReceivedArguments;
	TestTrue(TEXT("Register async tool"), Registry.RegisterAsyncTool(MakeDefinition(TEXT("test.async")),
		[&](const FMCPToolArguments& Arguments, FMCPToolCompletion Complete)
		{
			TestTrue(TEXT("Handler starts on game thread"), IsInGameThread());
			ReceivedArguments = Arguments;
			Completion = MoveTemp(Complete);
		}, Error));
	auto Arguments = MakeShared<FJsonObject>();
	Arguments->SetStringField(TEXT("value"), TEXT("original"));
	auto Future = Registry.CallTool(TEXT("test.async"), Arguments);
	TestFalse(TEXT("Queued call is not completed"), Future.IsReady());
	Arguments->SetStringField(TEXT("value"), TEXT("changed"));
	TestEqual(TEXT("Async arguments are a snapshot"), ReceivedArguments->GetStringField(TEXT("value")), FString(TEXT("original")));
	Registry.UnregisterTool(TEXT("test.async"));
	// This worker only resolves a promise; it does not require the game thread.
	Async(EAsyncExecution::ThreadPool, [Completion]()
	{
		Completion(FMCPToolResult::Success(TEXT("first")));
		Completion(FMCPToolResult::Failure(TEXT("duplicate")));
	}).Wait();
	TestTrue(TEXT("Worker completion resolves the future"), Future.IsReady());
	const auto Result = Future.Get();
	TestFalse(TEXT("First completion wins even after unregister"), Result.bIsError);
	TestEqual(TEXT("Original result retained"), Result.Content[0]->AsObject()->GetStringField(TEXT("text")), FString(TEXT("first")));

	Registry.RegisterAsyncTool(MakeDefinition(TEXT("test.pending")),
		[&](const FMCPToolArguments&, FMCPToolCompletion Complete) { Completion = MoveTemp(Complete); }, Error);
	auto Pending = Registry.CallTool(TEXT("test.pending"), MakeShared<FJsonObject>());
	Registry.Shutdown();
	TestTrue(TEXT("Shutdown resolves pending calls"), Pending.IsReady());
	TestTrue(TEXT("Shutdown reports failure"), Pending.Get().bIsError);
	Completion(FMCPToolResult::Success(TEXT("too late")));
	TestTrue(TEXT("Late completion cannot replace shutdown result"), Pending.Get().bIsError);
	auto AfterShutdown = Registry.CallTool(TEXT("test.pending"), MakeShared<FJsonObject>());
	TestTrue(TEXT("Calls after shutdown fail"), AfterShutdown.Get().bIsError);
	TestFalse(TEXT("Registration after shutdown fails"), Registry.RegisterAsyncTool(MakeDefinition(TEXT("late")),
		[](const FMCPToolArguments&, FMCPToolCompletion) {}, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPProtocolTest, "BSHarness.MCP.Protocol",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPProtocolTest::RunTest(const FString& Parameters)
{
	auto& Registry = FBSHarnessToolsModule::Get().GetToolRegistry();
	auto Request = MakeRequest(TEXT("tools/list"));
	auto Listed = FMCPToolDispatcher::Dispatch(Registry, Request).Get();
	TestEqual(TEXT("Preserve request ID"), Listed->GetStringField(TEXT("id")), FString(TEXT("test-request")));
	TestTrue(TEXT("Built-in tool is discoverable"), Listed->GetObjectField(TEXT("result"))->GetArrayField(TEXT("tools")).Num() >= 1);
	Request = MakeRequest(TEXT("tools/call"));
	auto Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("name"), TEXT("bsharness.editor_info"));
	Request->SetObjectField(TEXT("params"), Params);
	auto Called = FMCPToolDispatcher::Dispatch(Registry, Request).Get();
	auto Result = Called->GetObjectField(TEXT("result"));
	TestFalse(TEXT("Built-in call succeeds without arguments"), Result->GetBoolField(TEXT("isError")));
	TestTrue(TEXT("Returns engine version"), Result->GetObjectField(TEXT("structuredContent"))->HasField(TEXT("engineVersion")));
	Params->SetStringField(TEXT("name"), TEXT("missing"));
	auto Missing = FMCPToolDispatcher::Dispatch(Registry, Request).Get();
	TestEqual(TEXT("Unknown tool gives protocol error"), Missing->GetObjectField(TEXT("error"))->GetIntegerField(TEXT("code")), -32602);
	Params->SetStringField(TEXT("arguments"), TEXT("bad"));
	auto Invalid = FMCPToolDispatcher::Dispatch(Registry, Request).Get();
	TestEqual(TEXT("Invalid arguments rejected"), Invalid->GetObjectField(TEXT("error"))->GetIntegerField(TEXT("code")), -32602);
	Request->RemoveField(TEXT("id"));
	TestFalse(TEXT("Notification has no response"), FMCPToolDispatcher::Dispatch(Registry, Request).Get().IsValid());
	Request = MakeRequest(TEXT("TOOLS/LIST"));
	auto WrongCase = FMCPToolDispatcher::Dispatch(Registry, Request).Get();
	TestEqual(TEXT("Method names are case sensitive"), WrongCase->GetObjectField(TEXT("error"))->GetIntegerField(TEXT("code")), -32601);
	Request = MakeRequest(TEXT("tools/call"));
	Params = MakeShared<FJsonObject>();
	Params->SetNumberField(TEXT("name"), 42);
	Request->SetObjectField(TEXT("params"), Params);
	auto NumericName = FMCPToolDispatcher::Dispatch(Registry, Request).Get();
	TestEqual(TEXT("Tool name must be a JSON string"), NumericName->GetObjectField(TEXT("error"))->GetIntegerField(TEXT("code")), -32602);
	Request = MakeRequest(TEXT("unsupported"));
	auto Unknown = FMCPToolDispatcher::Dispatch(Registry, Request).Get();
	TestEqual(TEXT("Unknown method"), Unknown->GetObjectField(TEXT("error"))->GetIntegerField(TEXT("code")), -32601);
	Request->SetStringField(TEXT("jsonrpc"), TEXT("1.0"));
	auto Malformed = FMCPToolDispatcher::Dispatch(Registry, Request).Get();
	TestEqual(TEXT("Invalid envelope"), Malformed->GetObjectField(TEXT("error"))->GetIntegerField(TEXT("code")), -32600);

	FMCPToolRegistry LocalRegistry;
	FString Error;
	FMCPToolCompletion Complete;
	LocalRegistry.RegisterAsyncTool(MakeDefinition(TEXT("test.async")),
		[&](const FMCPToolArguments&, FMCPToolCompletion Callback) { Complete = MoveTemp(Callback); }, Error);
	Request = MakeRequest(TEXT("tools/call"));
	Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("name"), TEXT("test.async"));
	Request->SetObjectField(TEXT("params"), Params);
	auto AsyncResponse = FMCPToolDispatcher::Dispatch(LocalRegistry, Request);
	TestFalse(TEXT("Protocol waits for actual completion"), AsyncResponse.IsReady());
	Async(EAsyncExecution::ThreadPool, [Complete]() { Complete(FMCPToolResult::Failure(TEXT("operation failed"))); }).Wait();
	auto Failed = AsyncResponse.Get();
	TestFalse(TEXT("Execution failure is not a protocol error"), Failed->HasField(TEXT("error")));
	TestTrue(TEXT("Execution failure has isError"), Failed->GetObjectField(TEXT("result"))->GetBoolField(TEXT("isError")));
	return true;
}

class FMCPTimeoutCommand : public IAutomationLatentCommand
{
public:
	explicit FMCPTimeoutCommand(FAutomationTestBase* InTest) : Test(InTest)
	{
		Registry = MakeUnique<FMCPToolRegistry>();
		FString Error;
		Registry->RegisterAsyncTool(MakeDefinition(TEXT("test.timeout")),
			[this](const FMCPToolArguments&, FMCPToolCompletion Callback) { Complete = MoveTemp(Callback); }, Error);
		Future = Registry->CallTool(TEXT("test.timeout"), MakeShared<FJsonObject>(), 0.01);
		StartedAt = FPlatformTime::Seconds();
	}

	virtual bool Update() override
	{
		if (!Future.IsReady() && FPlatformTime::Seconds() - StartedAt < 5.0)
		{
			return false;
		}
		Test->TestTrue(TEXT("Uncompleted async call times out via ticker"), Future.IsReady());
		if (Future.IsReady())
		{
			Test->TestTrue(TEXT("Timeout reports failure"), Future.Get().bIsError);
			Complete(FMCPToolResult::Success(TEXT("late result")));
			Test->TestTrue(TEXT("Late callback cannot replace timeout"), Future.Get().bIsError);
		}
		Registry.Reset();
		return true;
	}

private:
	FAutomationTestBase* Test;
	TUniquePtr<FMCPToolRegistry> Registry;
	TFuture<FMCPToolResult> Future;
	FMCPToolCompletion Complete;
	double StartedAt = 0.0;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPTimeoutTest, "BSHarness.MCP.Timeout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPTimeoutTest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(FMCPTimeoutCommand(this));
	return true;
}

#endif
