# Mining and crafting UI prototype

The client UI now owns a survival overlay through `UVoxelSurvivalUISubsystem` in the existing Slate UI module. It waits until the front end hands control to the player, and also works in runs that skip the front end. Dedicated servers and editor preview worlds do not create this interface.

## Player flow

- Press **I** to open **Pack & Fieldcraft**. Escape or the Close button returns to the world. I remains typeable in the search field.
- The pack displays every actual inventory slot, including configurations above the default ten slots. Click a slot to select it. Selecting never consumes it.
- Item details show the registry name, stack capacity, and total count across stacks. Empty slots and an unavailable inventory are explicitly represented.
- **Throw selected & return** is available only for a nonempty throwable. It closes the panel, then calls the controller's existing `UseHotbarSlot`, retaining its debit, spawn, and refund behavior. This is an immediate throw, not a crafting action.
- The passive quick bar shows the first five inventory slots and their actual use keys **5–9**. Those existing bindings use items immediately; they are not remapped to Minecraft's selection semantics.
- The mining label reads `GetDigSizeVoxels()` and shows **millimeters per cube side**. It follows gameplay selection without maintaining a duplicate UI size setting. The default is **300 mm**; **2 / 3 / 4** select **100 / 200 / 300 mm** respectively. The footer displays these controls and the left-click mining action. The gameplay target outline previews the affected cube before mining.
- The handbook searches method titles, functional families, prerequisites, and downstream capabilities. Each entry shows all required prerequisites and explicitly labels alternative prerequisites as “One of.” All entries are labeled **Planned process**.

The world continues while the panel is open. Movement/look input is suppressed for the panel's lifetime and restored on close. The panel does not open during explosive charging. It does not pause multiplayer or simulation. The passive overlay is hit-test invisible and does not steal world clicks. Use `voxel.UI.Survival 0` to disable both surfaces during profiling or captures.

## Design influences

[Minecraft's official crafting guide](https://www.minecraft.net/en-us/article/how-craft) informs discoverable crafting references next to inventory. [Vintage Story's tools reference](https://wiki.vintagestory.at/Tools_and_Weapons) informs showing multi-step production and tool prerequisites. The interface uses the project's existing earth, iron, and gold palette and original text; no game art or layout has been copied.

The guide data comes from `docs/design/crafting-capabilities-v1.json`. Run `python tools/generate-crafting-handbook.py` after editing that proposal. `--check` verifies the committed generated `VoxelCraftingHandbook.inl` remains synchronized. This includes all 52 methods; it does not introduce a second manually maintained recipe list or require the source docs to ship with the game.

## Implemented boundaries

Inventory selection, counts, registry metadata, and throwable use are live. The current inventory is local, not replicated or saved by this UI. Runtime recipe execution, ingredient quantities, material suitability, tool capability checks, durability, workstations, and gathering drops are not implemented. A generic `block.rock` stack is not treated as suitable knappable stone. Planned prerequisites therefore do not display misleading green checks or “Craft” buttons. No fake items are seeded and no survival resources are awarded by the UI.

The next runtime step should provide recipe definitions and an authoritative `CanCraft`/`TryCraft` service. The UI should display the service's missing-resource/tool/station reasons and invoke a single transaction, rather than independently removing inputs across slots. Process and workshop interactions should be separate from instant assembly recipes. Visual item thumbnails can use reviewed Asset Forge renders once runtime item IDs are linked to the asset library.

## Validation

Static checks: generated handbook matches the graph; UI module internal-linkage collision lint passes. The shared Unreal Editor Development build passed on 2026-09-06. In the running 1600×900 standalone game, I opened the panel, searched `iron`, selected “Work hard mineral deposits,” verified wrapped prerequisite text and real inventory counts, and closed with Escape. Keys 2/3/4 changed both the live millimeter label and the visible cube outline to 100/200/300 mm. The default was 300 mm. Direct left-click carving of the prototype oak also succeeded and updated every LOD. Full movement/crouch restoration, throwing, resizing, menu-start flow, and multiplayer remain acceptance checks below.

Manual acceptance checks for that build:

1. Start through the menu and through the direct-game path. The quick bar appears only after gameplay begins.
2. Open with I while moving; verify movement and world dig/place stop. Search for `iron`, select a method, scroll requirements, and press Escape. Verify normal movement/look and cursor capture return.
3. Select empty and occupied slots; verify counts and limits match `voxel.Inventory.Dump`. Throw one existing cube using the button; verify exactly one debit and the normal projectile appears.
4. Exercise keys 5–9 and verify the quick bar changes with the real inventory. Change dig size and verify the millimeter readout follows it.
5. Use zero inventory seeding, a full inventory, and more than ten configured slots. Inspect all pack slots without clipping or phantom quantities.
6. Resize the viewport and inspect at 1280×720 and 1920×1080. The panel scales down to fit; long guide entries remain scrollable.
7. Disable `voxel.UI.Survival` while open, re-enable it, and travel/restart PIE. Verify no stranded mouse mode, duplicate input binding, or duplicated widgets.
