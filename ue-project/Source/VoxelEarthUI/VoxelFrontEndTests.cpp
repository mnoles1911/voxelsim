// Headless tests for the front end's pure logic.
//
// WHAT IS AND IS NOT COVERED. Almost all of a menu is pixels, and pixels are
// verified by the capture switches (docs/front-end-plan.md). What is left is a
// handful of small functions with invariants that are easy to state, easy to
// break, and INVISIBLE in a screenshot -- a progress bar that quietly goes
// backwards for two seconds looks fine in a still frame, and a slugify rule
// that maps an odd name onto an empty string writes into the saves root itself.
// Those are what this file covers.
//
// It is also the only part of this work that a machine with no display can run,
// which matters given the whole front end was written somewhere with no engine
// at all.
//
// Run headlessly:
//   UnrealEditor-Cmd.exe VoxelEarth.uproject -unattended -nullrhi -nop4 \
//     -ExecCmds="Automation RunTests VoxelEarth.FrontEnd; Quit"

#include "VoxelUITheme.h"
#include "VoxelWorldReadyProbe.h"
#include "VoxelSaveLibrary.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace VoxelFrontEndTestsDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.
//
// EAutomationTestFlags is a strongly-typed enum class in 5.8, not an int
// bitmask, so this must keep the enum type all the way through -- the same note
// VoxelSkyTests.cpp carries.
constexpr EAutomationTestFlags kTestFlags = EAutomationTestFlags::EditorContext
                                          | EAutomationTestFlags::ClientContext
                                          | EAutomationTestFlags::EngineFilter;
} // namespace VoxelFrontEndTestsDetail

// --- The progress model ------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelFrontEndProgressTest, "VoxelEarth.FrontEnd.LoadProgress",
                                 VoxelFrontEndTestsDetail::kTestFlags)

bool FVoxelFrontEndProgressTest::RunTest(const FString& Parameters)
{
	// 1. THE TIME FLOOR. With no work reported at all, the bar still advances,
	// because a bar that never moves reads as a hang. This is the term the
	// Godot original had, and the only one it had.
	TestEqual(TEXT("time floor with no work"), ComputeLoadProgress(0.5f, 0.f, 0.f, 0.f), 0.5f, 0.001f);

	// 2. WORK OVERTAKES TIME. Two seconds in on a warm cache, the world is
	// nearly there and the bar should say so rather than crawling to a 60 s
	// schedule -- the failure the original had in the other direction, reading
	// 30% at the moment the world was ready.
	const float WarmCache = ComputeLoadProgress(0.03f, 1.0f, 1.0f, 0.f);
	TestTrue(TEXT("work overtakes the time floor"), WarmCache > 0.9f);

	// 3. MONOTONE. RecomputeDesiredSet GROWS the desired set as the anchor
	// settles, so loaded/(loaded+outstanding) genuinely decreases mid-load.
	// The bar must not follow it down.
	const float High = ComputeLoadProgress(0.1f, 1.0f, 0.8f, 0.f);
	const float ThenWorse = ComputeLoadProgress(0.1f, 1.0f, 0.2f, High);
	TestEqual(TEXT("progress never decreases"), ThenWorse, High, 0.0001f);

	// 4. NEVER 1.0, however good the inputs look. Reaching 100% is the gate's
	// privilege; a timer expiring must not be able to claim it.
	TestTrue(TEXT("capped below one"), ComputeLoadProgress(1.0f, 1.0f, 1.0f, 1.0f) <= 0.995f);
	TestTrue(TEXT("capped below one, and near it"), ComputeLoadProgress(1.0f, 1.0f, 1.0f, 1.0f) > 0.99f);

	// 5. Out-of-range inputs are clamped rather than propagated. A caller
	// dividing by a zero denominator should produce a wrong-but-bounded bar,
	// not a NaN that paints a full-width fill.
	TestTrue(TEXT("negative inputs clamp"), ComputeLoadProgress(-1.f, -1.f, -1.f, 0.f) >= 0.f);
	TestTrue(TEXT("over-unity inputs clamp"), ComputeLoadProgress(5.f, 5.f, 5.f, 0.f) <= 0.995f);

	// 6. Ring fill dominates the work term. The spatial probe saturates as soon
	// as the ground under the spawn exists and then says nothing more, so a
	// full spatial term with an empty ring must NOT read as nearly done.
	const float SpatialOnly = ComputeLoadProgress(0.f, 1.0f, 0.f, 0.f);
	TestTrue(TEXT("spatial alone is not nearly-done"), SpatialOnly < 0.3f);

	return true;
}

// --- Godot's Color.darkened(), which is a byte operation ---------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelFrontEndDarkenTest, "VoxelEarth.FrontEnd.Darkened",
                                 VoxelFrontEndTestsDetail::kTestFlags)

bool FVoxelFrontEndDarkenTest::RunTest(const FString& Parameters)
{
	// THE POINT OF THIS TEST is that the obvious implementation is wrong.
	// Godot's Color.darkened(k) scales the STORED components, which for a
	// colour built from a hex literal are sRGB bytes -- so PANEL_OAK_2
	// darkened by 0.15 is #27170b. Doing the same scale in linear space gives a
	// visibly different colour, and both the pressed and disabled button states
	// are defined this way, so getting it wrong shows on every menu.
	const FColor Pressed = VoxelUITheme::Darkened(VoxelUITheme::PanelOak2, 0.15f);
	TestEqual(TEXT("pressed R"), int32(Pressed.R), int32(FMath::RoundToInt(0x2e * 0.85f)));
	TestEqual(TEXT("pressed G"), int32(Pressed.G), int32(FMath::RoundToInt(0x1b * 0.85f)));
	TestEqual(TEXT("pressed B"), int32(Pressed.B), int32(FMath::RoundToInt(0x0d * 0.85f)));

	// Alpha is carried through untouched -- Godot's does the same, and a
	// darkened button that also went translucent would be a surprise.
	TestEqual(TEXT("alpha preserved"), int32(Pressed.A), int32(VoxelUITheme::PanelOak2.A));

	// The ends behave.
	TestEqual(TEXT("zero darkening is identity"),
	          VoxelUITheme::Darkened(VoxelUITheme::Gold, 0.f).ToPackedARGB(), VoxelUITheme::Gold.ToPackedARGB());
	const FColor Black = VoxelUITheme::Darkened(VoxelUITheme::Gold, 1.0f);
	TestEqual(TEXT("full darkening is black"), int32(Black.R) + int32(Black.G) + int32(Black.B), 0);

	return true;
}

// --- Save-name slugification -------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelFrontEndSlugifyTest, "VoxelEarth.FrontEnd.Slugify",
                                 VoxelFrontEndTestsDetail::kTestFlags)

bool FVoxelFrontEndSlugifyTest::RunTest(const FString& Parameters)
{
	// The example from GameState.gd's own comment.
	TestEqual(TEXT("the documented case"), VoxelSave::Slugify(TEXT("My Save 1!")), TEXT("my_save_1"));

	// Runs collapse and the ends are trimmed, so a name cannot produce a slug
	// with leading or doubled separators.
	TestEqual(TEXT("runs collapse"), VoxelSave::Slugify(TEXT("  a   b  ")), TEXT("a_b"));
	TestEqual(TEXT("punctuation collapses"), VoxelSave::Slugify(TEXT("a---b")), TEXT("a_b"));

	// THE CASE THAT MATTERS MOST. An all-punctuation name would otherwise slug
	// to the empty string, and a save directory named "" is the saves ROOT --
	// which is a directory full of other people's saves.
	TestEqual(TEXT("empty becomes untitled"), VoxelSave::Slugify(TEXT("")), TEXT("untitled"));
	TestEqual(TEXT("all punctuation becomes untitled"), VoxelSave::Slugify(TEXT("!!!")), TEXT("untitled"));
	TestEqual(TEXT("whitespace becomes untitled"), VoxelSave::Slugify(TEXT("   ")), TEXT("untitled"));

	// Case folds, digits survive.
	TestEqual(TEXT("case folds"), VoxelSave::Slugify(TEXT("ABC")), TEXT("abc"));
	TestEqual(TEXT("digits survive"), VoxelSave::Slugify(TEXT("Day 12")), TEXT("day_12"));

	// A slug is idempotent: re-slugging one must not change it, or a save
	// written and then re-read under its own slug would drift to a new
	// directory each time.
	const FString Once = VoxelSave::Slugify(TEXT("Copper Isles: Day 1"));
	TestEqual(TEXT("idempotent"), VoxelSave::Slugify(Once), Once);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
