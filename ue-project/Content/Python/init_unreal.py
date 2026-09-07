"""Editor start-up hook. The PythonScriptPlugin runs <Project>/Content/Python/init_unreal.py
once, early, every time the full editor boots (never for -game, never for commandlets
that skip Python).

-VoxelAutoPIE on the EDITOR command line: start Play In Editor as soon as the level
editor is up, so "open the editor" lands on the game's title screen instead of an
empty Untitled level. The voxel world only exists in game worlds
(UVoxelWorldSubsystem::DoesSupportWorldType refuses editor worlds by design), so an
editor without PIE never shows terrain -- which is what the owner saw on 2026-09-07
and, reasonably, read as a fault.

WITHOUT the switch this file does nothing at all: Codex's editor launches and the
owner's own double-click stay exactly as they were. tools/voxel-editor.ps1 passes it.

The switch name contains none of VoxelFrontEndPolicy's self-driving substrings, so
the PIE session it starts is an "ordinary interactive launch" and gets the front end
(the title screen), not a fixture arm.
"""
import unreal

_SWITCH = "-VoxelAutoPIE"
# Seconds of Slate ticks to let the main frame, the asset registry and the
# untitled level settle before asking for a play session. The request is
# deferred to the editor tick either way; this only keeps it out of the
# start-up burst where the level-editor module may not have a viewport yet.
_SETTLE_SECONDS = 3.0

_state = {"handle": None, "elapsed": 0.0, "done": False}


def _wants_auto_pie():
    try:
        cmd = unreal.SystemLibrary.get_command_line()
    except Exception:  # noqa: BLE001 -- any failure here means "no"
        return False
    return any(tok.lower() == _SWITCH.lower() for tok in cmd.split())


def _finish():
    _state["done"] = True
    if _state["handle"] is not None:
        try:
            unreal.unregister_slate_post_tick_callback(_state["handle"])
        finally:
            _state["handle"] = None


def _tick(delta_seconds):
    if _state["done"]:
        return
    _state["elapsed"] += float(delta_seconds)
    if _state["elapsed"] < _SETTLE_SECONDS:
        return
    try:
        level_editor = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
        if level_editor.is_in_play_in_editor():
            unreal.log("VoxelAutoPIE: a play session is already running; nothing to do.")
        else:
            unreal.log("VoxelAutoPIE: requesting Play In Editor (%.1f s after the first Slate tick)."
                       % _state["elapsed"])
            level_editor.editor_request_begin_play()
    except Exception as exc:  # noqa: BLE001 -- report, never crash the editor's tick
        unreal.log_error("VoxelAutoPIE: could not start Play In Editor: %s" % exc)
    _finish()


if _wants_auto_pie():
    unreal.log("VoxelAutoPIE: armed by %s -- PIE starts once the level editor is up." % _SWITCH)
    _state["handle"] = unreal.register_slate_post_tick_callback(_tick)
