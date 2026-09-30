using UnrealBuildTool;

public class AgentWorkbenchEditor : ModuleRules
{
    public AgentWorkbenchEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject" });
        PrivateDependencyModuleNames.AddRange(new[] {
            "ApplicationCore", "AssetRegistry", "ContentBrowser", "DeveloperSettings", "Engine",
            "BSHarnessHooks", "BSHarnessTools", "GraphEditor", "HTTP", "InputCore", "Json", "Kismet", "LevelEditor",
            "MaterialEditor", "ModelContextProtocol", "Slate", "SlateCore", "ToolMenus",
            "ToolsetRegistry", "UnrealEd"
        });
    }
}
