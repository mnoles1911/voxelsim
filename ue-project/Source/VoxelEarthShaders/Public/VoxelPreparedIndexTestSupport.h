#if WITH_DEV_AUTOMATION_TESTS
#pragma once
#include "VoxelBrickPool.h"
// Explicit mock for tests of pool allocation rather than the production index.
// Never installed by production code; real attachment uses index reservations.
namespace VoxelPreparedIndexTestSupport
{
class FDelivery : public FVoxelBrickPreparedIndexDelivery
{
public:
    explicit FDelivery(FVoxelBrickIndexSink InSink):Sink(MoveTemp(InSink)){}
    bool ValidateForCommit() const override {return true;}
    void Commit(const FVoxelBrickIndexDelta& Delta) override {Sink(Delta);}
private:
    FVoxelBrickIndexSink Sink;
};
inline void SetSink(FVoxelBrickPool& Pool,FVoxelBrickIndexSink Sink,TArray<FVoxelBrickIndexEntry>& Initial)
{
    const auto Copy=Sink;
    Pool.SetIndexSink(MoveTemp(Sink),Initial,[Copy](const FVoxelBrickIndexDelta&){
        return MakeShared<FDelivery,ESPMode::ThreadSafe>(Copy);
    });
}
}
#endif
