#pragma once

#include "MCPToolRegistry.h"
#include "MCPToolConfiguration.h"
#include "Modules/ModuleManager.h"
#include "Misc/CoreMisc.h"

class BSHARNESSTOOLS_API FBSHarnessToolsModule : public IModuleInterface, public FSelfRegisteringExec
{
public:
	static FBSHarnessToolsModule& Get();
	FMCPToolRegistry& GetToolRegistry();
	bool ReloadToolConfiguration(FString& OutError);

	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
	// Async providers must drain their tasks before module unloading.
	virtual bool SupportsDynamicReloading() override { return false; }
	virtual bool Exec_Editor(UWorld* World, const TCHAR* Cmd, FOutputDevice& Ar) override;

private:
	TUniquePtr<FMCPToolRegistry> Registry;
	TArray<TFuture<void>> BackgroundTasks;
	TArray<FMCPToolBinding> NativeHandlers;
	FMCPToolConfiguration Configuration;
	FString ConfigurationPath;
};
