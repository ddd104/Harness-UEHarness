using UnrealBuildTool;

public class AgentWorkbenchEditor : ModuleRules
{
    public AgentWorkbenchEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject" });
        PrivateDependencyModuleNames.AddRange(new[] {
            "ApplicationCore", "AssetRegistry", "ContentBrowser", "DeveloperSettings", "Engine",
            "BSHarnessTools", "HTTP", "InputCore", "Json", "LevelEditor", "ModelContextProtocol",
            "Slate", "SlateCore", "ToolMenus", "ToolsetRegistry", "UnrealEd"
        });
    }
}
