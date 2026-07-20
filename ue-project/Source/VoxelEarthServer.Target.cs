using UnrealBuildTool;
using System.Collections.Generic;

// M3 wave 1 (docs/m3-plan.md decisions table, "Server model"): a dedicated-
// server target so `UnrealEditor-Cmd -server` / the standalone
// VoxelEarthServer binary can host the authoritative vxc::World + edit log
// with no client-side rendering modules pulled in. Listen-server play still
// goes through the normal VoxelEarthTarget (TargetType.Game) -- this target
// exists specifically for TargetType.Server (dedicated).
public class VoxelEarthServerTarget : TargetRules
{
	public VoxelEarthServerTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Server;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;

		ExtraModuleNames.Add("VoxelEarth");
	}
}
