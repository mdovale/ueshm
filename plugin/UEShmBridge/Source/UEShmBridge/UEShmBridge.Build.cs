using UnrealBuildTool;

public class UEShmBridge : ModuleRules
{
	public UEShmBridge(ReadOnlyTargetRules Target) : base(Target)
	{
		PrivateIncludePaths.Add(System.IO.Path.GetFullPath(System.IO.Path.Combine(ModuleDirectory, "../../../../common")));
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});
	}
}
