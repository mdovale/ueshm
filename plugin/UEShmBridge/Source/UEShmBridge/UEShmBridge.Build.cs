using UnrealBuildTool;

public class UEShmBridge : ModuleRules
{
	public UEShmBridge(ReadOnlyTargetRules Target) : base(Target)
	{
		PrivateDependencyModuleNames.Add("Core");
	}
}
