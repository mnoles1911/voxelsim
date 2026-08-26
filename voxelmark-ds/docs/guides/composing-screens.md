# Composing a VoxelMark screen

Every screen in this system is the same four-layer stack, and getting the stack
right is most of getting the look right.

```
VoxelMarkRoot          font, ink colour, square corners
└─ ScreenFrame         flat backdrop → photographic art → wash
   └─ content          a centred column, or a panel
```

`MainMenuScreen` and `LoadingScreen` are that stack pre-assembled. Reach for
them first; drop to `ScreenFrame` only when you are building a screen the game
does not have.

## The wash is conditional, and that is load-bearing

`ScreenFrame` lays a dark wash over background art -- 55% night-black on the
menu, 62% pure black while loading -- because the type and the oak panels are
not readable over raw photography.

**With no `background`, there is no wash.** The screen sits on its flat `#0a0a0f`
backdrop instead. Washing that would put near-black buttons on near-black, which
turns a graceful fallback into an unreadable screen. So: pick a backdrop, or
accept the flat one. Never hand-roll a wash over the flat backdrop.

## Panels want to sit on art

The oak panel's hard drop shadow exists to separate it from photography. On the
flat backdrop that shadow has almost nothing to do, and the panel reads as a
floating brown rectangle. If a design uses `Panel`, give the screen a backdrop.

## Disabled, not hidden

The menu shows all seven entries always. CONTINUE and LOAD GAME go *disabled*
when there are no saves rather than disappearing, so the menu a player sees is
the same menu every time. `MainMenuScreen` wires this to one `hasSaves` prop --
follow the same rule for anything you add.

## The gap above QUIT is not decoration

Menu entries sit 14px apart. QUIT sits 80px below the rest. That break is the
only thing standing between a misclick and quitting the game, and it should
survive any re-layout.

## Loading is a long screen, not a flash

A cold start runs tens of seconds. That is why the loading screen carries a
rotating quip, a rotating tip, and an animated hourglass at all -- the player
*will* read it. Two consequences worth keeping:

- The quip line reserves height for two lines even when it shows one, so a
  wrapping quip does not shove the bar up and down every 2.5 seconds.
- The percentage floors rather than rounds. 99.6% reads 99%, because a bar that
  says 100% while the world is still landing is exactly the lie the progress
  model exists to avoid.

## Type over art needs a shadow

`Text` takes `shadow="title"` (headings) or `shadow="body"` (everything else).
Over a backdrop, use one. The screen components already do.
