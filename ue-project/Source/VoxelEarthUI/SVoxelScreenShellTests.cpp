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

// THE LAYOUT SPACE A RESOLUTION ACTUALLY PRODUCES, and the reason this file can
// test "at 1080p" and "at 1440p" at all without a window.
//
// DefaultEngine.ini's [/Script/Engine.UserInterfaceSettings] sets
// UIScaleRule=ShortestSide on a curve whose keys are (480,0.444) (720,0.666)
// (1080,1.0) (8640,8.0) -- linear throughout and therefore exactly
// `shortestSide / 1080` at every point (ADR-0011 decision 2). Slate divides the
// viewport by that before any widget sees it, so this function is the whole of
// what a widget's local geometry is on a given screen.
//
// ITS OUTPUT IS THE POINT OF THE TEST BELOW: on ANY landscape display the
// shortest side is the height, so the height always comes back 1080 and 1080p
// and 1440p hand the shell the SAME space.
FVector2D LayoutUnitsFor(const FVector2D& ViewportPx, float InterfaceSize = 1.f)
{
	const double ShortestSide = FMath::Min(ViewportPx.X, ViewportPx.Y);
	const double EngineScale = ShortestSide / 1080.0;
	// INTERFACE SIZE multiplies the engine scale (VoxelGraphicsUserSettings::
	// GetUIScale through FSlateApplication::SetApplicationScale), so it divides
	// the space, which is why it -- and not the resolution -- is what makes the
	// shell run out of room.
	const double Total = EngineScale * double(FMath::Max(InterfaceSize, 0.01f));
	return FVector2D(ViewportPx.X / Total, ViewportPx.Y / Total);
}

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

// --- Responsive trimming (ADR-0011 decision 4) -------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVoxelScreenShellChromeTest, "VoxelEarth.FrontEnd.ScreenShell.Chrome",
                                 VoxelScreenShellTestsDetail::kTestFlags)

bool FVoxelScreenShellChromeTest::RunTest(const FString& Parameters)
{
	using namespace VoxelScreenShellTestsDetail;
	const FVoxelMenuLayout& L = FVoxelMenuLayout::Get();

	const FVector2D At1080 = LayoutUnitsFor(FVector2D(1920.0, 1080.0));
	const FVector2D At1440 = LayoutUnitsFor(FVector2D(2560.0, 1440.0));
	const FVector2D At4K   = LayoutUnitsFor(FVector2D(3840.0, 2160.0));

	// 1. THE ASSERTION THIS WHOLE FEATURE TURNS ON, and the one that refuted the
	// brief it was written from ("trim at 1080p, leave 1440p authored"). Under
	// ShortestSide anchored 1.0 at 1080, the layout space is 1080 units tall on
	// EVERY landscape display, so there is no such thing as a chrome that is
	// right at 1440p and wrong at 1080p. Measured as well as derived: the
	// before-captures in ue-project/Saved/ui-before are the same screen at both
	// sizes and the shell's body measures 1024 px of 1920 and 1366 px of 2560 --
	// 53.3% of the width in both.
	TestEqual(TEXT("1080p lays out in 1080 units of height"), float(At1080.Y), 1080.f, 0.001f);
	TestEqual(TEXT("1440p lays out in the SAME 1080 units of height"), float(At1440.Y), 1080.f, 0.001f);
	TestEqual(TEXT("4K lays out in the same 1080 units of height"), float(At4K.Y), 1080.f, 0.001f);
	TestEqual(TEXT("1080p and 1440p are the same layout width too"), float(At1440.X), float(At1080.X), 0.001f);

	// 2. SO THE CHROME IS THE AUTHORED ONE AT BOTH, and nothing about it varies
	// with the resolution. A future edit that keys the trim off pixels instead
	// of off the space the shell has fails here.
	const FVoxelShellChrome C1080 = L.ShellChromeForViewport(At1080, 1.f);
	const FVoxelShellChrome C1440 = L.ShellChromeForViewport(At1440, 1.f);
	TestFalse(TEXT("1080p at Menu Size 1.00 is not trimmed"), C1080.bTrimmed);
	TestFalse(TEXT("1440p at Menu Size 1.00 is not trimmed"), C1440.bTrimmed);
	TestEqual(TEXT("the frame is the authored 760 at 1080p"), C1080.ShellHeight, L.ScreenShellHeight, 0.01f);
	TestEqual(TEXT("the frame is the authored 760 at 1440p"), C1440.ShellHeight, L.ScreenShellHeight, 0.01f);
	TestEqual(TEXT("the top padding is authored at 1080p"), C1080.PadTop, L.ScreenShellPadTop, 0.01f);
	TestEqual(TEXT("the body padding is authored at 1080p"), C1080.BodyPadY, L.ScreenBodyPad, 0.01f);
	TestEqual(TEXT("the action-bar gap is authored at 1080p"), C1080.ActionBarTopGap, L.ActionBarTopGap, 0.01f);
	TestEqual(TEXT("the body content box is still 988 wide"), C1080.BodyContentWidth(),
	          L.ScreenBodyContentWidth(), 0.01f);

	// 3. WHAT ACTUALLY MAKES THE SPACE SHRINK. INTERFACE SIZE at its shipped
	// maximum divides the layout space by 1.5, so the viewport is 720 units tall
	// and the authored 760-unit frame does not fit -- at EVERY resolution, which
	// is the point. Before this trim the shell simply drew past the top and
	// bottom of the screen.
	const FVector2D Interface150 = LayoutUnitsFor(FVector2D(1920.0, 1080.0), 1.5f);
	TestEqual(TEXT("INTERFACE SIZE 1.50 leaves 720 units of height"), float(Interface150.Y), 720.f, 0.001f);
	TestTrue(TEXT("the authored frame does not fit at INTERFACE SIZE 1.50"),
	         L.ScreenShellHeight > float(Interface150.Y));
	const FVoxelShellChrome CBig = L.ShellChromeForViewport(Interface150, 1.f);
	TestTrue(TEXT("INTERFACE SIZE 1.50 trims the chrome"), CBig.bTrimmed);
	TestTrue(TEXT("and it fits afterwards"),
	         CBig.ShellHeight + 2.f * L.ShellViewportMargin <= float(Interface150.Y) + 0.01f);

	// 4. TIER ONE IS A TRADE OF CHROME FOR FRAME AND NOTHING ELSE. Menu Size
	// 1.40 at 1080 units leaves the frame 754 units of room; the trimmed frame
	// is 734, and every one of the 26 units it lost came off a padding, so the
	// body's content box is exactly the size it was. This is the assertion that
	// separates "trimmed" from "scaled down", which is the distinction the whole
	// of ADR-0011 decision 4 rests on.
	const FVoxelShellChrome CTier1 = L.ShellChromeForViewport(At1080, 1.40f);
	TestTrue(TEXT("Menu Size 1.40 trims"), CTier1.bTrimmed);
	TestFalse(TEXT("Menu Size 1.40 does not have to cut the body"), CTier1.bBodyShortened);
	const float ChromeGiven = (L.ScreenShellPadTop - CTier1.PadTop)
	                        + 2.f * (L.ScreenBodyPad - CTier1.BodyPadY)
	                        + (L.ActionBarTopGap - CTier1.ActionBarTopGap);
	TestEqual(TEXT("the frame lost exactly what the chrome gave"),
	          L.ScreenShellHeight - CTier1.ShellHeight, ChromeGiven, 0.01f);
	TestTrue(TEXT("the chrome actually gave something"), ChromeGiven > 0.f);

	// 5. THE BOTTOM PADDING IS NEVER TRIMMED, at any viewport. The gripper is
	// drawn inside it (ShellGripInset + 3 dots + 2 gaps), and test 7 of the
	// resize-zone case pins that relationship against ScreenShellPadBottom -- a
	// trim here would put the stair over the action bar's last hint.
	const FVoxelShellChrome CTiny = L.ShellChromeForViewport(FVector2D(1920.0, 300.0), 1.f);
	TestEqual(TEXT("the bottom padding survives the smallest viewport"),
	          CTiny.PadBottom, L.ScreenShellPadBottom, 0.01f);
	TestTrue(TEXT("the frame stops at its floor rather than inverting"),
	         CTiny.ShellHeight >= L.ShellMinHeight - 0.01f);

	// 6. NO GEOMETRY IS NOT A SMALL VIEWPORT. A widget's cached geometry is zero
	// until its first arrange; answering "trim everything" for that one frame
	// would open every screen with a visible twitch.
	const FVoxelShellChrome CUnarranged = L.ShellChromeForViewport(FVector2D::ZeroVector, 1.f);
	TestFalse(TEXT("an unarranged widget gets the authored chrome"), CUnarranged.bTrimmed);
	TestEqual(TEXT("and the authored height with it"), CUnarranged.ShellHeight, L.ScreenShellHeight, 0.01f);

	// 7. THE SHELL FITS AT EVERY MENU SIZE STOP, AT BOTH RESOLUTIONS. The reason
	// this test exists: at the shipped maximum of 1.50 the untrimmed shell wanted
	// 760 x 1.50 = 1140 units of the 1080 there are and hung 30 units off the top
	// and bottom of the screen -- on the owner's 1440p monitor exactly as much as
	// on a 1080p one. Stated as a loop over the setting's own stops so a change
	// to the range cannot outrun it.
	const FVector2D Viewports[] = {At1080, At1440};
	const TCHAR* Names[] = {TEXT("1920x1080"), TEXT("2560x1440")};
	for (int32 V = 0; V < 2; ++V)
	{
		for (float Stop = VoxelScreenShellSettings::ScaleMin();
		     Stop <= VoxelScreenShellSettings::ScaleMax() + 0.0001f;
		     Stop += VoxelScreenShellSettings::ScaleStep())
		{
			const float Snapped = VoxelScreenShellSettings::SnapScale(Stop);
			const FVoxelShellChrome Chrome = L.ShellChromeForViewport(Viewports[V], Snapped);
			const float PaintedH = Chrome.ShellHeight * Snapped;
			const float PaintedW = Chrome.ShellWidth * Snapped;
			TestTrue(*FString::Printf(TEXT("%s: the shell fits vertically at Menu Size %.2f"),
			                          Names[V], Snapped),
			         PaintedH + 2.f * L.ShellViewportMargin <= float(Viewports[V].Y) + 0.01f);
			TestTrue(*FString::Printf(TEXT("%s: the shell fits horizontally at Menu Size %.2f"),
			                          Names[V], Snapped),
			         PaintedW + 2.f * L.ShellViewportMargin <= float(Viewports[V].X) + 0.01f);
			// AND IT IS STILL A SHELL. A "fit" bought by collapsing the frame to
			// nothing would satisfy the two above and be useless.
			TestTrue(*FString::Printf(TEXT("%s: the frame is still usable at Menu Size %.2f"),
			                          Names[V], Snapped),
			         Chrome.ShellHeight >= L.ShellMinHeight - 0.01f);
		}
	}

	// 8. AND THE TRIM IS ONE-WAY. Every trimmed value is smaller than the
	// authored one it replaces, never larger -- a sign error here would grow the
	// chrome on exactly the screens that have no room for it.
	TestTrue(TEXT("the trimmed top padding is smaller"), L.TrimShellPadTop < L.ScreenShellPadTop);
	TestTrue(TEXT("the trimmed body padding is smaller"), L.TrimScreenBodyPadY < L.ScreenBodyPad);
	TestTrue(TEXT("the trimmed action-bar gap is smaller"), L.TrimActionBarTopGap < L.ActionBarTopGap);
	// ADR-0011 decision 3 still binds the trimmed set: nothing may become a
	// one-unit band.
	TestTrue(TEXT("no trimmed padding falls to a hairline"),
	         L.TrimShellPadTop >= VoxelUITheme::RulePx && L.TrimScreenBodyPadY >= VoxelUITheme::RulePx
	             && L.TrimActionBarTopGap >= VoxelUITheme::RulePx);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
