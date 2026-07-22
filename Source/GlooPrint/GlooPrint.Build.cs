using UnrealBuildTool;

public class GlooPrint : ModuleRules
{
    public GlooPrint(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Core", "CoreUObject", "Engine", "Slate", "SlateCore",
            "GraphEditor", "BlueprintGraph", "UnrealEd", "InputCore", "Settings"
        });
        if (Target.bBuildDeveloperTools)
        {
            PrivateDependencyModuleNames.AddRange(new[] { "Kismet", "AnimGraph" });
            PrivateDependencyModuleNames.Add("AutomationDriver");
        }
    }
}
