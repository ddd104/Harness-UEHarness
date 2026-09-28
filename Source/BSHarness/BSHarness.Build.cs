using UnrealBuildTool;

public class BSHarness : ModuleRules
{
	public BSHarness(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PrivateDependencyModuleNames.AddRange(new[] { "Core", "BSHarnessTools" });
	}
}
