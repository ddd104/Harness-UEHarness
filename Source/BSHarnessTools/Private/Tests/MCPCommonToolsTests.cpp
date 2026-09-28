#include "BSHarnessToolsModule.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Curves/CurveFloat.h"
#include "Misc/AutomationTest.h"
#include "Misc/StringOutputDevice.h"
#include "UObject/Package.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"

namespace
{
	FMCPToolResult CallBuiltin(FAutomationTestBase& Test, const TCHAR* Name, const TSharedRef<FJsonObject>& Arguments)
	{
		auto Future = FBSHarnessToolsModule::Get().GetToolRegistry().CallTool(Name, Arguments);
		// These specific file workers never depend on the game thread. Bound this test-only wait.
		if (!Future.WaitFor(FTimespan::FromSeconds(5)))
		{
			Test.AddError(FString::Printf(TEXT("Built-in call did not complete: %s"), Name));
			return FMCPToolResult::Failure(TEXT("Test wait expired."));
		}
		return Future.Get();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPBuiltinCatalogTest, "BSHarness.MCP.Builtins.CatalogAndValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPBuiltinCatalogTest::RunTest(const FString& Parameters)
{
	auto& Registry = FBSHarnessToolsModule::Get().GetToolRegistry();
	const auto Tools = Registry.ListTools();
	for (const TCHAR* Name : {TEXT("bsharness.editor_info"), TEXT("bsharness.project_info"), TEXT("bsharness.assets.search"),
		TEXT("bsharness.assets.get"), TEXT("bsharness.assets.open"), TEXT("bsharness.actors.list"), TEXT("bsharness.source.list"), TEXT("bsharness.source.read")})
	{
		TestTrue(FString::Printf(TEXT("Tool registered: %s"), Name), Tools.ContainsByPredicate(
			[&](const FMCPToolDefinition& Tool) { return Tool.Name.Equals(Name, ESearchCase::CaseSensitive); }));
	}
	auto Arguments = MakeShared<FJsonObject>();
	const auto Project = CallBuiltin(*this, TEXT("bsharness.project_info"), Arguments);
	TestFalse(TEXT("Project info succeeds"), Project.bIsError);
	if (Project.StructuredContent)
	{
		TestFalse(TEXT("Project name available"), Project.StructuredContent->GetStringField(TEXT("projectName")).IsEmpty());
		TestTrue(TEXT("Plugin source root available"), Project.StructuredContent->HasField(TEXT("pluginSource")));
	}
	Arguments->SetStringField(TEXT("unexpected"), TEXT("value"));
	TestEqual(TEXT("Unknown argument rejected"), CallBuiltin(*this, TEXT("bsharness.project_info"), Arguments).ProtocolErrorCode.Get(0), -32602);
	Arguments = MakeShared<FJsonObject>();
	Arguments->SetNumberField(TEXT("limit"), 0.5);
	TestEqual(TEXT("Fractional limit rejected"), CallBuiltin(*this, TEXT("bsharness.assets.search"), Arguments).ProtocolErrorCode.Get(0), -32602);
	Arguments->SetNumberField(TEXT("limit"), 501);
	TestEqual(TEXT("Large limit rejected"), CallBuiltin(*this, TEXT("bsharness.actors.list"), Arguments).ProtocolErrorCode.Get(0), -32602);
	Arguments = MakeShared<FJsonObject>();
	Arguments->SetStringField(TEXT("recursive"), TEXT("true"));
	TestEqual(TEXT("String boolean rejected"), CallBuiltin(*this, TEXT("bsharness.source.list"), Arguments).ProtocolErrorCode.Get(0), -32602);
	Arguments = MakeShared<FJsonObject>();
	Arguments->SetStringField(TEXT("root"), TEXT("engine"));
	TestEqual(TEXT("Unknown source root rejected"), CallBuiltin(*this, TEXT("bsharness.source.list"), Arguments).ProtocolErrorCode.Get(0), -32602);
	TestEqual(TEXT("Required asset path checked"), CallBuiltin(*this, TEXT("bsharness.assets.get"), MakeShared<FJsonObject>()).ProtocolErrorCode.Get(0), -32602);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPBuiltinEditorTest, "BSHarness.MCP.Builtins.EditorQueries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPBuiltinEditorTest::RunTest(const FString& Parameters)
{
	// In-memory fixture only: no existing asset changes and no package saved to disk.
	const FString Folder = TEXT("/Game/__BSHarnessMCP_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UPackage* Package = CreatePackage(*(Folder / TEXT("Fixture")));
	UCurveFloat* Asset = NewObject<UCurveFloat>(Package, TEXT("Fixture"), RF_Public | RF_Standalone);
	FAssetRegistryModule::AssetCreated(Asset);
	auto Arguments = MakeShared<FJsonObject>();
	Arguments->SetStringField(TEXT("path"), Folder);
	Arguments->SetNumberField(TEXT("limit"), 1);
	const auto Search = CallBuiltin(*this, TEXT("bsharness.assets.search"), Arguments);
	TestFalse(TEXT("Asset search succeeds"), Search.bIsError);
	if (Search.StructuredContent)
	{
		const auto& Items = Search.StructuredContent->GetArrayField(TEXT("items"));
		TestEqual(TEXT("Finds fixture"), Items.Num(), 1);
		if (!Items.IsEmpty())
		{
			TestEqual(TEXT("Returns actual object path"), Items[0]->AsObject()->GetStringField(TEXT("objectPath")), Asset->GetPathName());
		}
	}
	Arguments->SetNumberField(TEXT("offset"), MAX_int32);
	const auto EmptyPage = CallBuiltin(*this, TEXT("bsharness.assets.search"), Arguments);
	if (EmptyPage.StructuredContent)
	{
		TestTrue(TEXT("Out-of-range offset gives an empty page without overflow"), EmptyPage.StructuredContent->GetArrayField(TEXT("items")).IsEmpty());
	}
	Arguments = MakeShared<FJsonObject>();
	Arguments->SetStringField(TEXT("object_path"), Asset->GetPathName());
	const auto Detail = CallBuiltin(*this, TEXT("bsharness.assets.get"), Arguments);
	TestFalse(TEXT("Asset details succeed"), Detail.bIsError);
	if (Detail.StructuredContent)
	{
		TestEqual(TEXT("Asset class matches"), Detail.StructuredContent->GetStringField(TEXT("classPath")), Asset->GetClass()->GetPathName());
		TestTrue(TEXT("Dependencies included"), Detail.StructuredContent->HasField(TEXT("dependencies")));
	}
	Arguments->SetStringField(TEXT("object_path"), Folder / TEXT("Missing.Missing"));
	const auto Missing = CallBuiltin(*this, TEXT("bsharness.assets.get"), Arguments);
	TestTrue(TEXT("Missing asset reports execution failure"), Missing.bIsError && !Missing.ProtocolErrorCode.IsSet());
	FAssetRegistryModule::AssetDeleted(Asset);
	Asset->ClearFlags(RF_Public | RF_Standalone);
	Asset->MarkAsGarbage();
	Package->SetDirtyFlag(false);
	Package->MarkAsGarbage();

	Arguments = MakeShared<FJsonObject>();
	Arguments->SetStringField(TEXT("name_contains"), FGuid::NewGuid().ToString());
	const auto Actors = CallBuiltin(*this, TEXT("bsharness.actors.list"), Arguments);
	TestFalse(TEXT("Editor actor query succeeds"), Actors.bIsError);
	if (Actors.StructuredContent)
	{
		TestTrue(TEXT("Unmatched actor filter returns empty list"), Actors.StructuredContent->GetArrayField(TEXT("items")).IsEmpty());
		TestTrue(TEXT("Editor world identified"), Actors.StructuredContent->HasField(TEXT("world")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPBuiltinSourceTest, "BSHarness.MCP.Builtins.SourceQueries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPBuiltinSourceTest::RunTest(const FString& Parameters)
{
	auto Arguments = MakeShared<FJsonObject>();
	Arguments->SetStringField(TEXT("path"), TEXT("BSHarnessTools"));
	Arguments->SetNumberField(TEXT("limit"), 1);
	const auto Files = CallBuiltin(*this, TEXT("bsharness.source.list"), Arguments);
	TestFalse(TEXT("Source enumeration succeeds"), Files.bIsError);
	if (Files.StructuredContent)
	{
		const auto& Items = Files.StructuredContent->GetArrayField(TEXT("items"));
		TestEqual(TEXT("Limit applied"), Items.Num(), 1);
		TestTrue(TEXT("Source results paginated"), Files.StructuredContent->GetBoolField(TEXT("hasMore")));
		if (!Items.IsEmpty())
		{
			TestTrue(TEXT("Paths relative to source root"), Items[0]->AsString().StartsWith(TEXT("BSHarnessTools/")));
		}
	}
	Arguments = MakeShared<FJsonObject>();
	Arguments->SetStringField(TEXT("path"), TEXT("BSHarnessTools/BSHarnessTools.Build.cs"));
	Arguments->SetNumberField(TEXT("max_lines"), 3);
	const auto Source = CallBuiltin(*this, TEXT("bsharness.source.read"), Arguments);
	TestFalse(TEXT("Source read succeeds"), Source.bIsError);
	if (Source.StructuredContent)
	{
		const auto& Lines = Source.StructuredContent->GetArrayField(TEXT("lines"));
		TestEqual(TEXT("Line limit applied"), Lines.Num(), 3);
		if (Lines.Num() == 3)
		{
			TestEqual(TEXT("Correct first source line"), Lines[0]->AsString(), FString(TEXT("using UnrealBuildTool;")));
			TestTrue(TEXT("Blank source lines preserved"), Lines[1]->AsString().IsEmpty());
		}
		TestEqual(TEXT("Next line is one based"), Source.StructuredContent->GetIntegerField(TEXT("nextLine")), 4);
	}
	Arguments->SetNumberField(TEXT("start_line"), MAX_int32);
	const auto PastEnd = CallBuiltin(*this, TEXT("bsharness.source.read"), Arguments);
	if (PastEnd.StructuredContent)
	{
		TestTrue(TEXT("Line past EOF returns empty list"), PastEnd.StructuredContent->GetArrayField(TEXT("lines")).IsEmpty());
	}
	Arguments = MakeShared<FJsonObject>();
	for (const TCHAR* Path : {TEXT("../BSHarness.uplugin"), TEXT("C:/Windows/win.ini"), TEXT(".. /BSHarness.uplugin"), TEXT("file.txt:stream"), TEXT("test.uasset")})
	{
		Arguments->SetStringField(TEXT("path"), Path);
		TestEqual(FString::Printf(TEXT("Unsupported source path rejected: %s"), Path),
			CallBuiltin(*this, TEXT("bsharness.source.read"), Arguments).ProtocolErrorCode.Get(0), -32602);
	}
	Arguments->SetStringField(TEXT("path"), TEXT("__missing_source__.cpp"));
	const auto Missing = CallBuiltin(*this, TEXT("bsharness.source.read"), Arguments);
	TestTrue(TEXT("Missing source file reports execution failure"), Missing.bIsError && !Missing.ProtocolErrorCode.IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPConsoleTest, "BSHarness.MCP.Builtins.Console",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPConsoleTest::RunTest(const FString& Parameters)
{
	auto& Module = FBSHarnessToolsModule::Get();
	FStringOutputDevice Output;
	TestTrue(TEXT("Handler discovery command handled"), Module.Exec_Editor(nullptr, TEXT("BSHarness.MCP.Handlers"), Output));
	TestTrue(TEXT("Handler metadata printed"), Output.Contains(TEXT("assets.search")) && Output.Contains(TEXT("configurationFile")));
	TestTrue(TEXT("Reload command handled"), Module.Exec_Editor(nullptr, TEXT("BSHarness.MCP.Reload"), Output));
	TestTrue(TEXT("Default config reload succeeds"), Output.Contains(TEXT("Loaded 8 tools")));
	TestTrue(TEXT("List command handled"), Module.Exec_Editor(nullptr, TEXT("BSHarness.MCP.List"), Output));
	TestTrue(TEXT("JSON arguments preserved"), Module.Exec_Editor(nullptr,
		TEXT("BSHarness.MCP.Call bsharness.assets.search {\"name_contains\":\"name with spaces\",\"limit\":1}"), Output));
	TestTrue(TEXT("Raw request handled"), Module.Exec_Editor(nullptr,
		TEXT("BSHarness.MCP.Request {\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/list\"}"), Output));
	TestTrue(TEXT("Malformed envelope handled by dispatcher"), Module.Exec_Editor(nullptr, TEXT("BSHarness.MCP.Request {}"), Output));
	TestTrue(TEXT("Invalid input produces usage"), Module.Exec_Editor(nullptr, TEXT("BSHarness.MCP.Call bsharness.project_info {bad}"), Output));
	TestTrue(TEXT("Useful usage text"), Output.Contains(TEXT("Usage:")));
	TestFalse(TEXT("Unrelated command left alone"), Module.Exec_Editor(nullptr, TEXT("stat fps"), Output));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPBuiltinOpenAssetTest, "BSHarness.MCP.Builtins.OpenAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPBuiltinOpenAssetTest::RunTest(const FString& Parameters)
{
	auto Arguments = MakeShared<FJsonObject>();
	TestEqual(TEXT("Open requires an asset path"), CallBuiltin(*this, TEXT("bsharness.assets.open"), Arguments).ProtocolErrorCode.Get(0), -32602);
	Arguments->SetStringField(TEXT("object_path"), TEXT("C:/not-an-asset.uasset"));
	TestEqual(TEXT("Filesystem path rejected"), CallBuiltin(*this, TEXT("bsharness.assets.open"), Arguments).ProtocolErrorCode.Get(0), -32602);
	const FString PackagePath = TEXT("/Game/__BSHarnessOpen_") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT("/Curve");
	Arguments->SetStringField(TEXT("object_path"), PackagePath);
	TestTrue(TEXT("Missing asset fails"), CallBuiltin(*this, TEXT("bsharness.assets.open"), Arguments).bIsError);

	UPackage* Package = CreatePackage(*PackagePath);
	UCurveFloat* Asset = NewObject<UCurveFloat>(Package, TEXT("Curve"), RF_Public | RF_Standalone);
	FAssetRegistryModule::AssetCreated(Asset);
	UAssetEditorSubsystem* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	const auto Opened = CallBuiltin(*this, TEXT("bsharness.assets.open"), Arguments);
	TestFalse(TEXT("Package path opens the asset"), Opened.bIsError);
	TestTrue(TEXT("Actual asset editor exists"), Editors->FindEditorForAsset(Asset, false) != nullptr);
	if (Opened.StructuredContent)
	{
		TestTrue(TEXT("Open confirmed"), Opened.StructuredContent->GetBoolField(TEXT("opened")));
		TestFalse(TEXT("First call creates editor"), Opened.StructuredContent->GetBoolField(TEXT("alreadyOpen")));
		TestEqual(TEXT("Path normalized"), Opened.StructuredContent->GetStringField(TEXT("objectPath")), Asset->GetPathName());
	}
	Arguments->SetStringField(TEXT("object_path"), FString::Printf(TEXT("/Script/Engine.CurveFloat'%s'"), *Asset->GetPathName()));
	const auto Again = CallBuiltin(*this, TEXT("bsharness.assets.open"), Arguments);
	TestFalse(TEXT("Copy Reference syntax accepted"), Again.bIsError);
	if (Again.StructuredContent)
	{
		TestTrue(TEXT("Existing editor reused"), Again.StructuredContent->GetBoolField(TEXT("alreadyOpen")));
	}
	Package->SetDirtyFlag(false);
	Editors->CloseAllEditorsForAsset(Asset);
	FAssetRegistryModule::AssetDeleted(Asset);
	Asset->ClearFlags(RF_Public | RF_Standalone);
	Asset->MarkAsGarbage();
	Package->MarkAsGarbage();
	return true;
}

#endif
