# Building with VoxelMark

This is a game front end, not a web app kit: a medieval, square-cornered,
parchment-on-oak language ported from VoxelMark's own menu and loading screens.
Everything here already exists in the shipping game — match it rather than
inventing around it.

## Wrap everything in `VoxelMarkRoot`

```jsx
<VoxelMarkRoot>
  <MainMenuScreen background="castleFeast" hasSaves />
</VoxelMarkRoot>
```

`VoxelMarkRoot` applies the Macondo Swash Caps face, the parchment ink colour
and the square-corner baseline. **A component outside it still renders — in the
host page's font and colour, with no error.** That silent failure is the single
most common way a VoxelMark screen comes out wrong, so start every design with
the root.

## Style with `--vm-*` tokens and `vm-*` classes

There is no Tailwind here. Your own layout markup uses this vocabulary, and only
names in it resolve:

| Family | Pattern | Real examples |
|---|---|---|
| Background | `vm-bg-<token>` | `vm-bg-panel-oak1`, `vm-bg-panel-oak2`, `vm-bg-menu-backdrop`, `vm-bg-bg-stone` |
| Ink | `vm-text-<token>` | `vm-text-gold`, `vm-text-ink`, `vm-text-ink-dim`, `vm-text-ink-mute`, `vm-text-hp-bright` |
| Border colour | `vm-border-<token>` | `vm-border-gold`, `vm-border-iron-deep` |
| Type scale | `vm-type-<step>` | `vm-type-title`, `vm-type-loading`, `vm-type-panel-title`, `vm-type-button`, `vm-type-quip`, `vm-type-body`, `vm-type-row`, `vm-type-tip`, `vm-type-stamp` |
| Spacing | `vm-gap-<name>` / `vm-p-<name>` | `vm-gap-row`, `vm-gap-sub`, `vm-gap-column`, `vm-gap-panel`, `vm-gap-loading`, `vm-gap-title`, `vm-gap-quit` |
| Structure | — | `vm-border-hard` (2px black), `vm-border-thin`, `vm-stack`, `vm-row`, `vm-center`, `vm-fill` |
| Text shadow | — | `vm-shadow-title`, `vm-shadow-body`, `vm-shadow-tight` |

Every colour class has a matching custom property for inline use:
`var(--vm-gold)`, `var(--vm-panel-oak1)`, `var(--vm-ink-dim)`,
`var(--vm-sand-bright)`, `var(--vm-font-serif)`. Layout numbers are tokens too:
`var(--vm-button-min-height)`, `var(--vm-loading-bar-width)`.

**No rounded corners, anywhere.** The root resets `border-radius` to 0 on
everything inside it; a rounded element reads instantly as not-this-game.

## Rules that carry meaning, not taste

- **Text over background art needs a shadow.** `<Text shadow="title">` for
  headings, `shadow="body"` for everything else.
- **Disabled, never hidden.** CONTINUE and LOAD GAME go disabled when there are
  no saves so the menu is always the same menu.
- **The 80px gap above QUIT** (`vm-gap-quit`) is what stops a misclick quitting
  the game. Every other entry is 14px apart.
- **Art implies a wash; no art means no wash.** `ScreenFrame` handles this —
  washing the flat `#0a0a0f` backdrop would put near-black on near-black. Don't
  hand-roll one.
- **Panels want to sit on art.** The hard, un-blurred drop shadow exists to
  separate them from photography.

## Where the truth is

Read `_ds/<folder>/styles.css` and the `_ds_bundle.css` it imports — the token
block is at the top of that file, and every class above is defined there. Per
component, read its `.prompt.md`. `guidelines/composing-screens.md` covers how
the two screens are assembled.

## A typical build

```jsx
<VoxelMarkRoot>
  <ScreenFrame background="cave" wash="menu">
    <div className="vm-center" style={{ width: '100%', height: '100%' }}>
      <Panel style={{ width: 520 }}>
        <Text as="h2" tone="title" size="panel-title">SAVE AND QUIT?</Text>
        <Text as="p" tone="dim" size="body" style={{ marginTop: 10 }}>
          Progress since your last rest will be kept.
        </Text>
        <div className="vm-row vm-gap-sub" style={{ marginTop: 16 }}>
          <MenuButton size="dialog">SAVE</MenuButton>
          <MenuButton size="dialog" destructive>DISCARD</MenuButton>
        </div>
      </Panel>
    </div>
  </ScreenFrame>
</VoxelMarkRoot>
```
