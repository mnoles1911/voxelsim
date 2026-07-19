using UnrealBuildTool;
using System.Collections.Generic;

public class VoxelEarthTarget : TargetRules
{
	public VoxelEarthTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;

		ExtraModuleNames.Add("VoxelEarth");
	}
}
