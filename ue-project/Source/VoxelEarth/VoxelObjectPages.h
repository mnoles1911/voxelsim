#pragma once
#include "VoxelObjectRegistry.h"
namespace VoxelObjectPages
{
// Call on the game thread after residency processing. At most two worker jobs
// per world are outstanding. No filesystem work occurs on the game thread.
void Tick(UWorld* World,const TArray<VoxelObjects::FView>& Views);
void Drain(UWorld* World);
void Forget(UWorld* World);
// Worker-only immutable snapshot materialization. Original entry remains valid
// on failure. Save and network encoding must resolve pages before dereferencing.
bool Hydrate(VoxelObjects::FEntry& Snapshot);
// Pure worker IO seam used by automation; writes content-addressed page bytes.
bool Write(const FString& Directory,const VoxelObjects::FGeometry& Geometry,VoxelObjects::FGeometryPage& Page);
bool Read(const VoxelObjects::FGeometryPage& Page,VoxelObjects::FGeometry& Geometry);
bool PublishWrite(VoxelObjects::FRegistry& Registry,const FGuid& Id,uint64 GeometryRevision,const VoxelObjects::FGeometry& Original,const VoxelObjects::FGeometryPage& Page);
bool PublishRead(VoxelObjects::FRegistry& Registry,const FGuid& Id,uint64 GeometryRevision,const VoxelObjects::FGeometryPage& Page,const VoxelObjects::FGeometry& Loaded);
}
