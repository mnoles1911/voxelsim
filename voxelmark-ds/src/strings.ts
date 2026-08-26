// The front end's copy, ported from VoxelUIStrings.cpp.
//
// These are here because a screen built with placeholder text is not the screen
// -- the loading quips in particular set the game's voice, and a design that
// substitutes "Loading..." for them looks like a different product.

export const TITLE = 'VOXELMARK';
export const SUBTITLE = 'Mira-Thal Trilogy · Game One';
export const VERSION_STAMP = 'Milestone 5-3D — dev build';

/** The loading screen's heading. The spaces are literal -- it fakes letter-spacing. */
export const LOADING_TITLE = 'L O A D I N G';
export const TIP_PREFIX = 'TIP';

/** The main menu's entries, in the order the game lists them. */
export const MENU_ITEMS = [
  'CONTINUE',
  'NEW GAME',
  'LOAD GAME',
  'SETTINGS',
  'HELP',
  'CREDITS',
  'QUIT',
] as const;

/**
 * The loading quips, in source order. The game shuffles them per show and
 * crossfades one to the next every 2.5 seconds.
 */
export const LOADING_QUIPS = [
  'Pillaging villages...',
  'Organizing goblin bands...',
  'Conjuring sorcerer spells...',
  'Inviting pirates to the royal feast...',
  'Sharpening dwarven axes...',
  "Lighting the ash-throne's braziers...",
  'Forging cursed blades...',
  'Plucking arrows from corpses...',
  "Counting the king's gold (twice)...",
  "Polishing the executioner's block...",
  'Whispering rumours in tavern corners...',
  'Teaching wolves to read maps...',
  'Reminding the Aelorin who they were...',
  'Bargaining with the dwindling dead...',
  'Stoking the volcano under Drûn-Khazad...',
  "Rehearsing Roland's funeral oration...",
  'Apologizing to the goats...',
  'Bribing the night watch...',
  'Translating goblin curses...',
  'Salting the fields after harvest...',
  'Drafting unfair trade agreements...',
  'Misremembering the prophecy...',
  'Pouring mead for the long-dead...',
  'Stealing songs from minstrels...',
] as const;

/** The gameplay tips shown in the loading screen's footer, rotating every 8 seconds. */
export const GAMEPLAY_TIPS = [
  'Press [E] to talk to NPCs. Most have things to do.',
  'Edits to the world persist. The pit you dug last week is still there.',
  'Hold attack longer for a heavier swing — at the cost of stamina.',
  'Lock-on with [RMB]. Useful when one-vs-many.',
  "Settlements are protected. The world won't yield inside their walls.",
  'Water flows. If you carve under a pond, expect a small flood.',
  'Save anywhere from the pause menu. Rest at a fire to autosave.',
  "Lethe's Draught lets you re-spec — once. Spend it carefully.",
  'Rain dampens fire. Wet bowstrings misfire. Dress for the weather.',
  'The compass points north. The sun rises east. The map is hand-drawn.',
  'You can throw most things. Sometimes that solves the problem.',
  'Roland flinches when low. The HUD rarely lies, but his body never does.',
  "Press Q / E to cycle quick slots. Shovels won't break stone.",
] as const;
