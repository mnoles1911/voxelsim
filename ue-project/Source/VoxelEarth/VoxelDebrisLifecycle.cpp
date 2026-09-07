#include "VoxelDebrisLifecycle.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "TimerManager.h"
#include "VoxelEarth.h"

UVoxelDebrisLifecycle::UVoxelDebrisLifecycle() { PrimaryComponentTick.bCanEverTick = false; }
void UVoxelDebrisLifecycle::BeginPlay() { Super::BeginPlay(); Arm(); }
void UVoxelDebrisLifecycle::EndPlay(const EEndPlayReason::Type Reason)
{
    if (GetWorld()) GetWorld()->GetTimerManager().ClearTimer(CleanupTimer);
    Super::EndPlay(Reason);
}
void UVoxelDebrisLifecycle::Configure(EVoxelDebrisLifetime Kind)
{
    State.Kind = Kind;
    State.RemainingSeconds = Kind == EVoxelDebrisLifetime::Cosmetic ? 10. : 900.;
    if (HasBegunPlay()) Arm();
}
void UVoxelDebrisLifecycle::Retain() { Configure(EVoxelDebrisLifetime::Retained); }
void UVoxelDebrisLifecycle::NotifyInteraction()
{
    if (State.Kind == EVoxelDebrisLifetime::Harvestable) { State.RemainingSeconds = 900.; LastSample = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.; }
}
bool UVoxelDebrisLifecycle::Advance(FVoxelDebrisLifetimeState& Value, double Seconds, bool Protected)
{
    if (Value.Kind == EVoxelDebrisLifetime::Substantial || Value.Kind == EVoxelDebrisLifetime::Retained) return false;
    if (Value.Kind == EVoxelDebrisLifetime::Harvestable && Protected) return false;
    if (FMath::IsFinite(Seconds) && Seconds > 0.) Value.RemainingSeconds = FMath::Max(0., Value.RemainingSeconds - Seconds);
    return Value.RemainingSeconds <= 0.;
}
bool UVoxelDebrisLifecycle::ProtectsBounds(const FBox& Bounds, const FVector& Eye, const FVector& Forward)
{
    // Broad conservative cone: no occlusion queries or terrain raycasts. Include
    // the object's radius so large/edge-of-screen fragments remain protected.
    if (Bounds.ComputeSquaredDistanceToPoint(Eye) <= FMath::Square(2000.)) return true;
    const FVector To = Bounds.GetCenter() - Eye;
    const double Radius = Bounds.GetExtent().Size(), Distance = To.Size();
    return Distance - Radius <= 12000. && FVector::DotProduct(To, Forward.GetSafeNormal()) + Radius >= Distance * .5;
}
bool UVoxelDebrisLifecycle::IsProtected() const
{
    if (!GetWorld() || !GetOwner()) return true;
    FVector Center, Extent; GetOwner()->GetActorBounds(false, Center, Extent);
    const FBox Bounds(Center - Extent, Center + Extent);
    for (auto It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
    {
        auto PC = It->Get(); if (!PC) continue;
        FVector Eye; FRotator View; PC->GetPlayerViewPoint(Eye, View);
        if (ProtectsBounds(Bounds, Eye, View.Vector())) return true;
    }
    return false;
}
void UVoxelDebrisLifecycle::Arm()
{
    auto W = GetWorld(); if (!W) return;
    W->GetTimerManager().ClearTimer(CleanupTimer);
    LastSample = W->GetTimeSeconds();
    if (State.Kind == EVoxelDebrisLifetime::Substantial || State.Kind == EVoxelDebrisLifetime::Retained) return;
    if (State.Kind != EVoxelDebrisLifetime::Cosmetic && (W->GetNetMode()==NM_Client || !GetOwner()->HasAuthority())) return;
    // Cosmetics expire at their deadline. Resources use a low-frequency check,
    // independent of actor/physics tick and unaffected by physics sleep.
    const float Period = State.Kind == EVoxelDebrisLifetime::Cosmetic ? float(FMath::Max(.001, State.RemainingSeconds)) : .5f;
    W->GetTimerManager().SetTimer(CleanupTimer, this, &UVoxelDebrisLifecycle::Poll, Period, true);
}
void UVoxelDebrisLifecycle::Accrue()
{
    const double Now = GetWorld()->GetTimeSeconds();
    Advance(State, Now - LastSample, State.Kind == EVoxelDebrisLifetime::Harvestable && IsProtected());
    LastSample = Now;
}
void UVoxelDebrisLifecycle::Poll()
{
    if (!IsValid(GetOwner())) return;
    Accrue();
    if (State.RemainingSeconds <= 0. && (State.Kind == EVoxelDebrisLifetime::Cosmetic || !IsProtected()))
    {
        UE_LOG(LogVoxelEarth, Log, TEXT("DebrisCleanup expired %s kind=%d"), *GetOwner()->GetName(), int(State.Kind));
        GetOwner()->Destroy();
    }
    else if (State.Kind == EVoxelDebrisLifetime::Cosmetic)
    {
        // TimerManager and world time can differ slightly at frame boundaries.
        // Retry the remainder, never another full 10-second interval.
        Arm();
    }
}
FVoxelDebrisLifetimeState UVoxelDebrisLifecycle::CaptureState() const
{
    auto Copy = State;
    if (GetWorld()) Advance(Copy, GetWorld()->GetTimeSeconds() - LastSample, State.Kind == EVoxelDebrisLifetime::Harvestable && IsProtected());
    return Copy;
}
void UVoxelDebrisLifecycle::RestoreState(const FVoxelDebrisLifetimeState& Saved)
{
    State = Saved;
    if (uint8(State.Kind) > uint8(EVoxelDebrisLifetime::Retained)) State.Kind = EVoxelDebrisLifetime::Retained;
    const double Max = State.Kind == EVoxelDebrisLifetime::Cosmetic ? 10. : 900.;
    State.RemainingSeconds = FMath::IsFinite(State.RemainingSeconds) ? FMath::Clamp(State.RemainingSeconds, 0., Max) : Max;
    if (HasBegunPlay()) Arm();
}
