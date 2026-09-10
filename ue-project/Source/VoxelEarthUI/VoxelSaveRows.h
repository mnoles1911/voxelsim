#pragma once
// Turning VoxelSave::List() into the rows a list widget draws.
//
// ONE BUILDER, TWO SCREENS. The title screen's LOAD panel and the pause
// overlay's LOAD dialog show the same saves, and before this there was one copy
// of the conversion (in UVoxelFrontEndSubsystem::RefreshSaveRows) and about to
// be a second. The interesting part is not the loop -- it is the two judgements
// inside it, each of which took a comment to justify and neither of which
// should ever be made differently on two screens:
//
//   * COORDINATES ARE PRINTED IN METRES. Godot stored metres and this project
//     stores Unreal units, so the raw number would be -6510200 and read as
//     noise. Metres is what the F1 overlay and every log line use.
//
//   * A SAVE UNDER A DIFFERENT SEED IS NOT LOADABLE, and says so instead of
//     silently doing nothing when clicked. The seed is baked into the amplifier
//     long before any menu exists.

#include "CoreMinimal.h"
#include "SVoxelLoadDialog.h" // FVoxelSaveRowInfo

namespace VoxelSaveRows
{
// Newest first, which is VoxelSave::List()'s own contract and what makes row 0
// the LATEST tag's row and CONTINUE's target.
//
// RunningSeed is the seed this session was built with; 0 means "unknown", which
// suppresses the seed check rather than marking every row unloadable.
VOXELEARTHUI_API TArray<FVoxelSaveRowInfo> Build(uint64 RunningSeed);
} // namespace VoxelSaveRows
