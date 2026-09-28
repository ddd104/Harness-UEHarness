using UnrealBuildTool;

public class AgentWorkbenchEditor : ModuleRules
{
    public AgentWorkbenchEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject" });
        PrivateDependencyModuleNames.AddRange(new[] {
            "ApplicationCore", "AssetRegistry", "ContentBrowser", "DeveloperSettings", "Engine",
            "HTTP", "InputCore", "Json", "LevelEditor", "Slate", "SlateCore", "ToolMenus"
        });
    }
}
