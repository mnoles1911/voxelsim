// Headless tests for the unified screen shell's two 2026-09-08 repairs: the
// authored widths that stop a screen being silently scaled down, and the
// arithmetic behind the drag-resize grip.
//
// WHY THESE TWO THINGS AND NOTHING ELSE. Almost all of a menu is pixels, and
// pixels are judged on a capture by the owner (see VoxelFrontEndTests.cpp's
// header for the same argument). What is left here is a handful of numbers with
// invariants that are easy to state, easy to break in a refactor, and -- this is
// the part that matters -- INVISIBLE when they break.
//
// The journal defect this file was written after is exactly that shape. Nothing
// about the journal looked wrong in the source: every font size still matched
// the mock to the point. The screen simply asked for more width than the shell
// had, the shell's down-only fit believed it, and the whole page was drawn at
// 0.62. No assertion anywhere could have failed, because the arithmetic that
// went wrong was never written down. It is written down now, in
// FVoxelMenuLayout's derived accessors, and this file is what makes changing one
// of the shell figures without the others a red test instead of a small screen.
//
// Run headlessly:
//   UnrealEditor-Cmd.exe VoxelEarth.uproject -unattended -nullrhi -nop4 \
//     -ExecCmds="Automation RunTests VoxelEarth.FrontEnd.ScreenShell; Quit"

#include "SVoxelScreenShell.h"
#include "VoxelScreenShellSettings.h"
#include "VoxelUITheme.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace VoxelScreenShellTestsDetail
{
// Named namespace, not anonymous: see tools/lint-unity-collisions.py.
constexpr EAutomationTestFlags kTestFlags = EAutomationTestFlags::EditorContext
                                          | EAutomationTestFlags::ClientContext
                                          | EAutomationTestFlags::EngineFilter;

// A 1080p-equivalent widget space. The shell is added with
// AddViewportWidgetContent and its centring box fills the viewport, so this is
// the size the hit region actually sees; Slate's own DPI scale has already been
// divided out by the time a widget reads its local geometry.
const FVector2D kLocalSize(1920.0, 1080.0);
const FVector2D kCentre(960.0, 540.0);

// THE DRAG MAPPING, RESTATED. This is the one line SVoxelScreenShell::OnMouseMove
// runs, and repeating it here tests ResizeRatioAt and SnapScale rather than the
// widget's event wiring -- which needs a pointer, a capture and a viewport, and
// is therefore the owner's to judge on a live window. Said plainly so nobody
// reads a green run here as "the drag works".
float DragScaleFor(SVoxelScreenShell::EResizeZone Zone, const FVector2D& GrabPos, const FVector2D& NowPos,
                   float GrabScale)
{
	const float GrabRatio = SVoxelScreenShell::ResizeRatioAt(Zone, GrabPos, kLocalSize);
	const float Ratio = SVoxelScreenShell::ResizeRatioAt(Zone, NowPos, kLocalSize);
	return VoxelScreenShellSettings::SnapScale(GrabScale + (Ratio - GrabRatio));
}
} // namespace VoxelScreenShellTestsDetail

// --- The authored widths the shell hands a screen ----------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScreenShellLayoutTest, "VoxelEarth.FrontEnd.ScreenShell.Layout",
                                 VoxelScreenShellTestsDetail::kTestFlags)

bool FVoxelScreenShellLayoutTest::RunTest(const FString& Parameters)
{
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	// 1. THE BODY BOX IS THE FRAME MINUS ITS TWO PADDINGS. 1060 - 2*18 - 2*18.
	// Stated as a number as well as a formula on purpose: the formula alone
	// would still pass if every figure in it moved together, and the point of
	// the assertion is that the shell is the size ADR-0011 authored it at.
	TestEqual(TEXT("the shell is the authored 1060 wide"), L.ScreenShellWidth, 1060.f, 0.01f);
	TestEqual(TEXT("the shell is the authored 760 tall"), L.ScreenShellHeight, 760.f, 0.01f);
	TestEqual(TEXT("the body content box is 988 wide"), L.ScreenBodyContentWidth(), 988.f, 0.01f);

	// 2. EACH SCREEN'S COLUMNS EXACTLY FILL THAT BOX. This is the property the
	// journal defect violated: a screen whose columns sum to MORE than the body
	// is not clipped, it is scaled, and a scaled screen looks like a design
	// decision rather than a bug. Sum to less and the page is narrower than the
	// mock's `1fr`, which is a visible loss of a text column.
	TestEqual(TEXT("journal: list + gap + page fills the body exactly"),
	          L.JournalListWidth + L.JournalColumnGap + L.JournalPageWidth(),
	          L.ScreenBodyContentWidth(), 0.01f);
	TestEqual(TEXT("codex: three columns and two gaps fill the body exactly"),
	          L.CodexCategoryWidth + L.CodexColumnGap + L.CodexEntryListWidth + L.CodexColumnGap
	              + L.CodexPageWidth(),
	          L.ScreenBodyContentWidth(), 0.01f);

	// 3. AND THE RESULTING PAGES ARE THE MOCK'S. 988 - 300 - 18 and
	// 988 - 200 - 260 - 28. A page that came out at, say, 40 units would satisfy
	// (2) and still be useless, so the width itself is pinned.
	TestEqual(TEXT("the journal page is 670 wide"), L.JournalPageWidth(), 670.f, 0.01f);
	TestEqual(TEXT("the codex page is 500 wide"), L.CodexPageWidth(), 500.f, 0.01f);

	// 4. A PAGE MUST STILL HOLD A LINE OF PROSE. The parchment chrome and the
	// 28-unit page padding come off the width before any text is drawn; the
	// measured worst case at the journal's 18 px body is ~1221 units on one
	// unwrapped line, so the column has to be wide enough to wrap it into a
	// readable number of lines rather than a ribbon.
	const float JournalTextColumn = L.JournalPageWidth() - 2.f * L.JournalPagePadX - 2.f * VoxelUITheme::RulePx * 2.f;
	TestTrue(TEXT("the journal's text column clears 500 units"), JournalTextColumn > 500.f);

	return true;
}

// --- The resize grip's hit region --------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScreenShellResizeZoneTest, "VoxelEarth.FrontEnd.ScreenShell.ResizeZone",
                                 VoxelScreenShellTestsDetail::kTestFlags)

bool FVoxelScreenShellResizeZoneTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScreenShellTestsDetail;
	using EZone = SVoxelScreenShell::EResizeZone;
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	const double HalfW = double(L.ScreenShellWidth) * 0.5;
	const double HalfH = double(L.ScreenShellHeight) * 0.5;
	const double Right = kCentre.X + HalfW;
	const double Bottom = kCentre.Y + HalfH;

	// 1. THE MIDDLE OF THE SHELL IS NOT A HANDLE. If this ever comes back
	// anything but None, every click inside an open screen starts a resize.
	TestTrue(TEXT("the centre is not a resize zone"),
	         SVoxelScreenShell::ResizeZoneAt(kCentre, kLocalSize, 1.f) == EZone::None);
	TestTrue(TEXT("well inside the frame is not a resize zone"),
	         SVoxelScreenShell::ResizeZoneAt(FVector2D(Right - 60.0, Bottom - 60.0), kLocalSize, 1.f)
	             == EZone::None);

	// 2. THE THREE HANDLES, each on its own edge.
	TestTrue(TEXT("the right edge is a left-right handle"),
	         SVoxelScreenShell::ResizeZoneAt(FVector2D(Right, kCentre.Y), kLocalSize, 1.f) == EZone::RightEdge);
	TestTrue(TEXT("the bottom edge is an up-down handle"),
	         SVoxelScreenShell::ResizeZoneAt(FVector2D(kCentre.X, Bottom), kLocalSize, 1.f) == EZone::BottomEdge);
	TestTrue(TEXT("the bottom-right corner is a corner handle"),
	         SVoxelScreenShell::ResizeZoneAt(FVector2D(Right, Bottom), kLocalSize, 1.f) == EZone::Corner);

	// 3. THE CORNER IS REACHABLE FROM OUTSIDE IT. A player aiming at a corner
	// overshoots it, and a handle that only exists on the inside reads as one
	// that does not work.
	TestTrue(TEXT("just outside the corner is still the corner"),
	         SVoxelScreenShell::ResizeZoneAt(FVector2D(Right + L.ShellResizeEdge * 0.5,
	                                                   Bottom + L.ShellResizeEdge * 0.5),
	                                         kLocalSize, 1.f) == EZone::Corner);

	// 4. THE BAND IS A BAND, NOT A HALF-PLANE. One band-width inside the edge is
	// still a handle; three is not. Without the second of these the grip would
	// swallow clicks meant for the action bar and the body panel.
	TestTrue(TEXT("one band inside the right edge is still a handle"),
	         SVoxelScreenShell::ResizeZoneAt(FVector2D(Right - L.ShellResizeEdge * 0.9, kCentre.Y),
	                                         kLocalSize, 1.f) == EZone::RightEdge);
	TestTrue(TEXT("three bands inside the right edge is not"),
	         SVoxelScreenShell::ResizeZoneAt(FVector2D(Right - L.ShellResizeEdge * 3.0, kCentre.Y),
	                                         kLocalSize, 1.f) == EZone::None);

	// 5. THE HANDLE FOLLOWS THE MENU SIZE. This is the assertion that catches a
	// grip wired to the authored rectangle instead of the painted one -- the
	// failure a player meets as "it stopped working after I made it smaller",
	// which is invisible at the shipped default of 1.00.
	const double SmallRight = kCentre.X + HalfW * 0.75;
	TestTrue(TEXT("at 0.75 the old edge is no longer a handle"),
	         SVoxelScreenShell::ResizeZoneAt(FVector2D(Right, kCentre.Y), kLocalSize, 0.75f) == EZone::None);
	TestTrue(TEXT("at 0.75 the handle has moved in with the frame"),
	         SVoxelScreenShell::ResizeZoneAt(FVector2D(SmallRight, kCentre.Y), kLocalSize, 0.75f)
	             == EZone::RightEdge);

	// 6. THE BAND NEVER REACHES THE BODY PANEL, at any size the dial allows. The
	// band is measured inward from the frame edge and the shell's own padding is
	// what stands between it and .menu-body's chrome; at the smallest Menu Size
	// that padding is drawn at 0.75 of its authored value.
	TestTrue(TEXT("the band stays inside the shell's padding at the smallest size"),
	         L.ShellResizeEdge < L.ScreenShellPadX * VoxelScreenShellSettings::ScaleMin());

	// 7. THE GRIPPER FITS IN THE CORNER IT IS DRAWN IN. Inset plus three dots
	// plus two gaps must not exceed the shell's bottom padding, or the stair is
	// drawn over the action bar's last hint.
	const float GripExtent = L.ShellGripInset + 3.f * L.ShellGripDot + 2.f * L.ShellGripGap;
	TestTrue(TEXT("the gripper fits inside the shell's bottom padding"),
	         GripExtent <= L.ScreenShellPadBottom + 1.f);
	// ADR-0011: no band may be one unit.
	TestTrue(TEXT("no part of the gripper is thinner than a rule"),
	         L.ShellGripDot >= VoxelUITheme::RulePx && L.ShellGripGap >= VoxelUITheme::RulePx);

	return true;
}

// --- The drag mapping --------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScreenShellResizeDragTest, "VoxelEarth.FrontEnd.ScreenShell.ResizeDrag",
                                 VoxelScreenShellTestsDetail::kTestFlags)

bool FVoxelScreenShellResizeDragTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScreenShellTestsDetail;
	using EZone = SVoxelScreenShell::EResizeZone;
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	const double HalfW = double(L.ScreenShellWidth) * 0.5;
	const double HalfH = double(L.ScreenShellHeight) * 0.5;
	const FVector2D RightEdge(kCentre.X + HalfW, kCentre.Y);
	const FVector2D Corner(kCentre.X + HalfW, kCentre.Y + HalfH);

	// 1. THE RATIO IS 1.0 AT THE AUTHORED EDGE, on every axis and at the corner.
	// This is the unit the whole drag is expressed in: the ratio IS the scale
	// the shell would have to be for the frame's edge to sit under the pointer.
	TestEqual(TEXT("the right edge is ratio 1"),
	          SVoxelScreenShell::ResizeRatioAt(EZone::RightEdge, RightEdge, kLocalSize), 1.f, 0.001f);
	TestEqual(TEXT("the bottom edge is ratio 1"),
	          SVoxelScreenShell::ResizeRatioAt(EZone::BottomEdge,
	                                           FVector2D(kCentre.X, kCentre.Y + HalfH), kLocalSize),
	          1.f, 0.001f);
	TestEqual(TEXT("the corner is ratio 1"),
	          SVoxelScreenShell::ResizeRatioAt(EZone::Corner, Corner, kLocalSize), 1.f, 0.001f);

	// 2. A CORNER DRAG TAKES THE DOMINANT AXIS, so the corner never falls behind
	// the cursor on either one.
	const FVector2D WideOfCorner(kCentre.X + HalfW * 1.4, kCentre.Y + HalfH * 1.1);
	TestEqual(TEXT("a corner drag follows the axis that has moved further"),
	          SVoxelScreenShell::ResizeRatioAt(EZone::Corner, WideOfCorner, kLocalSize), 1.4f, 0.001f);

	// 3. GRABBING CHANGES NOTHING. The drag is additive, so at the instant of
	// the grab the delta is zero whatever the pointer caught -- a grab a few
	// units inside the band must not make the shell jump so the edge lands under
	// the cursor. This is the assertion an absolute mapping fails.
	const FVector2D GrabbedInside(kCentre.X + HalfW - L.ShellResizeEdge * 0.8, kCentre.Y);
	TestEqual(TEXT("a grab inside the band does not move the shell"),
	          DragScaleFor(EZone::RightEdge, GrabbedInside, GrabbedInside, 1.f), 1.f, 0.001f);
	TestEqual(TEXT("a grab at 1.25 does not move the shell either"),
	          DragScaleFor(EZone::RightEdge, GrabbedInside, GrabbedInside, 1.25f), 1.25f, 0.001f);

	// 4. DRAGGING OUT GROWS AND DRAGGING IN SHRINKS, by the amount the pointer
	// moved as a share of the authored half-extent.
	const FVector2D OutTenPercent(kCentre.X + HalfW * 1.1, kCentre.Y);
	const FVector2D InTenPercent(kCentre.X + HalfW * 0.9, kCentre.Y);
	TestEqual(TEXT("dragging out by a tenth grows by a tenth"),
	          DragScaleFor(EZone::RightEdge, RightEdge, OutTenPercent, 1.f), 1.10f, 0.001f);
	TestEqual(TEXT("dragging in by a tenth shrinks by a tenth"),
	          DragScaleFor(EZone::RightEdge, RightEdge, InTenPercent, 1.f), 0.90f, 0.001f);

	// 5. THE DRAG IS CLAMPED TO THE SAME RANGE THE SETTINGS ROW ALLOWS. A drag
	// that could reach a size the Menu Size slider cannot express would leave
	// the row unable to show what the player is looking at.
	const FVector2D FarOut(kCentre.X + HalfW * 4.0, kCentre.Y);
	const FVector2D FarIn(kCentre.X, kCentre.Y);
	TestEqual(TEXT("dragging far out stops at the maximum"),
	          DragScaleFor(EZone::RightEdge, RightEdge, FarOut, 1.f),
	          VoxelScreenShellSettings::ScaleMax(), 0.001f);
	TestEqual(TEXT("dragging far in stops at the minimum"),
	          DragScaleFor(EZone::RightEdge, RightEdge, FarIn, 1.f),
	          VoxelScreenShellSettings::ScaleMin(), 0.001f);

	// 6. WHAT THE DRAG PAINTS IS A VALUE THE SETTING CAN HOLD. The shell draws
	// the uncommitted drag value and stores it on release; if the two snapped
	// differently the shell would settle onto a different size at the end of the
	// gesture than the one the player was watching.
	for (int32 Step = 0; Step <= 20; ++Step)
	{
		const FVector2D At(kCentre.X + HalfW * (0.6 + 0.05 * double(Step)), kCentre.Y);
		const float Painted = DragScaleFor(EZone::RightEdge, RightEdge, At, 1.f);
		TestEqual(TEXT("a painted drag value survives its own commit"),
		          VoxelScreenShellSettings::SnapScale(Painted), Painted, 0.0001f);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
