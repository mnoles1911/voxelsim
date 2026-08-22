using UnrealBuildTool;
using System.Collections.Generic;

public class VoxelEarthEditorTarget : TargetRules
{
	public VoxelEarthEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;

		ExtraModuleNames.Add("VoxelEarth");
		ExtraModuleNames.Add("VoxelEarthShaders");
		// The front end (main menu + loading screen). ClientOnly, so
		// VoxelEarthServer.Target.cs deliberately does NOT list it -- a
		// dedicated server links no Slate at all.
		ExtraModuleNames.Add("VoxelEarthUI");
	}
}
