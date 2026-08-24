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
#include "VoxelFrontEndPolicy.h"

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

// --- Rule 5's switch classifier ----------------------------------------------
//
// WHY THIS TEST EXISTS. Rule 5 decides whether a run gets a main menu by
// looking for 14 substrings ANYWHERE in a -Voxel* switch name. That is a
// deliberate fuzzy rule with an asymmetric error budget (see
// VoxelFrontEndPolicy.cpp), and the fuzziness is the right call -- but it had
// no test, and the tool meant to police it cannot see this class of mistake:
// lint-frontend-switch-coverage.py SKIPS every name the substring rule already
// matches, so a name matched BY ACCIDENT is invisible to it by construction.
//
// Of 366 -Voxel* names in the tree, 81 match rule 5. Most deserve to. The ones
// that do not are ordinary knobs and instruments that merely inherited a
// fixture's vocabulary, and each one silently costs its user the menu.
//
// This test pins the two directions that matter. It is cheap, it is headless,
// and it fails loudly if somebody deletes an exemption -- which is the property
// the exemption table needs in order to be worth having.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelFrontEndSwitchPolicyTest, "VoxelEarth.FrontEnd.SwitchPolicy",
                                 VoxelFrontEndTestsDetail::kTestFlags)

bool FVoxelFrontEndSwitchPolicyTest::RunTest(const FString& Parameters)
{
	using VoxelFrontEnd::IsSelfDrivingSwitchName;

	// Direction 1: the rule still does its job. These are real fixtures, and a
	// menu in front of any of them is a hung capture -- the expensive failure.
	TestTrue(TEXT("a *Test fixture is self-driving"), IsSelfDrivingSwitchName(TEXT("VoxelGICaveTest")));
	TestTrue(TEXT("a *Shot capture is self-driving"), IsSelfDrivingSwitchName(TEXT("VoxelVistaShot")));
	TestTrue(TEXT("a timed *After is self-driving"), IsSelfDrivingSwitchName(TEXT("VoxelScreenshotAfter")));
	TestTrue(TEXT("a named extra is self-driving"), IsSelfDrivingSwitchName(TEXT("VoxelPerfRun")));
	TestTrue(TEXT("mid-name Shot still counts"), IsSelfDrivingSwitchName(TEXT("VoxelHudShotOnly")));

	// A switch that did not exist when the rule was written must still classify
	// from its name alone. This is the property that makes a rule better than a
	// list, so it is worth pinning rather than assuming.
	TestTrue(TEXT("an unknown *Test classifies from the convention alone"),
	         IsSelfDrivingSwitchName(TEXT("VoxelSomethingNobodyHasWrittenYetTest")));

	// Direction 2: the accidental match. -VoxelReadyProbeLog contains "Probe"
	// but is a log flag for FVoxelWorldReadyProbe -- the loading screen's gate,
	// which exists ONLY while the front end is up. Classified self-driving, it
	// suppressed the front end, which removed the loading screen, which removed
	// the probe: the diagnostic switch guaranteed its own subject never ran.
	//
	// Delete the exemption in VoxelFrontEndPolicy.cpp and this line fails --
	// which is the point of writing it down.
	TestFalse(TEXT("-VoxelReadyProbeLog is a log flag, not a fixture"),
	          IsSelfDrivingSwitchName(TEXT("VoxelReadyProbeLog")));

	// The other twelve accidents, spot-checked across all three substrings that
	// produced them. Every one of these is recorded ACCIDENTAL in
	// tools/frontend-switch-classification.txt; this is the half that makes the
	// recording true of the running game rather than only of the lint.
	//
	// -VoxelGpuMeshInFlight is the case worth keeping forever: it already ENDS
	// in the substring that caught it, so the advice the lint used to print --
	// "rename it to end in Test/Shot/After and the rule handles it" -- cannot
	// fix this class of mistake, and a suffix convention cannot encode intent.
	TestFalse(TEXT("a job in-flight cap is not a fixture"),
	          IsSelfDrivingSwitchName(TEXT("VoxelGpuMeshInFlight")));
	TestFalse(TEXT("a per-core job cap is not a fixture"),
	          IsSelfDrivingSwitchName(TEXT("VoxelJobsInFlightPerCore")));

	// The correctness arms. These verify DURING an ordinary run and never end
	// one, so they are exactly the switches a person arms interactively -- which
	// is the only situation where rule 5 is reached at all, since rule 4
	// (unattended) short-circuits every capture leg before rule 5 runs.
	TestFalse(TEXT("the solid-skip correctness arm is not a fixture"),
	          IsSelfDrivingSwitchName(TEXT("VoxelVerifySolidSkip")));
	TestFalse(TEXT("the buried-skip correctness arm is not a fixture"),
	          IsSelfDrivingSwitchName(TEXT("VoxelVerifyBuriedSkip")));
	TestFalse(TEXT("the sky-band correctness arm is not a fixture"),
	          IsSelfDrivingSwitchName(TEXT("VoxelVerifySkyBand")));
	TestFalse(TEXT("a worklist byte gate is not a fixture"),
	          IsSelfDrivingSwitchName(TEXT("VoxelGpuWorklistVerifyCT")));
	TestFalse(TEXT("the eviction-index exit-scan arm is not a fixture"),
	          IsSelfDrivingSwitchName(TEXT("VoxelBucketedExitScanVerify")));

	// An exemption must not leak to a NEIGHBOURING name. The table matches whole
	// names, not substrings, and a genuine fixture that merely shares a prefix
	// with an exempted switch must still be caught.
	TestTrue(TEXT("an exemption does not spread to a longer name"),
	         IsSelfDrivingSwitchName(TEXT("VoxelVerifySolidSkipTest")));

	// Ordinary configuration must never be caught. These are the plain cases
	// the rule gets right, and they are cheap insurance against a future
	// substring being added that is too greedy to be safe.
	TestFalse(TEXT("a seed is not self-driving"), IsSelfDrivingSwitchName(TEXT("VoxelSeed")));
	TestFalse(TEXT("a spawn pose is not self-driving"), IsSelfDrivingSwitchName(TEXT("VoxelSpawnAt")));
	TestFalse(TEXT("the front-end switch itself is not self-driving"), IsSelfDrivingSwitchName(TEXT("VoxelFrontEnd")));

	// The match is case-sensitive by construction; a lowercase spelling is a
	// different switch and must not inherit the classification.
	TestFalse(TEXT("classification is case-sensitive"), IsSelfDrivingSwitchName(TEXT("Voxelgicavetest")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
