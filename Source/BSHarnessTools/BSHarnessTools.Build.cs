using UnrealBuildTool;

public class BSHarnessTools : ModuleRules
{
	public BSHarnessTools(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new[] { "Core", "Json" });
		PrivateDependencyModuleNames.AddRange(new[] { "CoreUObject", "Engine", "UnrealEd", "AssetRegistry", "Projects" });
		RuntimeDependencies.Add("$(PluginDir)/Config/MCPTools.json", StagedFileType.NonUFS);
	}
}
