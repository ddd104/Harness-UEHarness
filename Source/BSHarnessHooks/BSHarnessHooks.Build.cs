using UnrealBuildTool;

public class BSHarnessHooks : ModuleRules
{
    public BSHarnessHooks(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "Json", "BSHarnessTools" });
    }
}
