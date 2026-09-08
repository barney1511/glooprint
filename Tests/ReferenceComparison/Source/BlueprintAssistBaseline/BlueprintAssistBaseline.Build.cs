using System.IO;
using UnrealBuildTool;

public class BlueprintAssistBaseline : ModuleRules
{
    public BlueprintAssistBaseline(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "Slate", "SlateCore", "InputCore",
            "UnrealEd", "Kismet", "BlueprintGraph", "GraphEditor", "BlueprintAssist"
        });
        PrivateIncludePaths.Add(Path.GetFullPath(Path.Combine(Target.ProjectFile.Directory.FullName, "../../../../Source/GlooPrint/Private")));
    }
}
