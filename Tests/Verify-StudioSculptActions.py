"""Exercise the Sculpting header's Draw, Smooth, and Add Sculpt Detail actions with undo and redo."""

from pathlib import Path
import runpy
import sys
import traceback

import bpy


exchange, bridge = (Path(value) for value in sys.argv[sys.argv.index("--") + 1:][:2])
authoring = runpy.run_path(str(bridge))
window = bpy.context.window
target = bpy.data.workspaces["Sculpting"]
window.workspace = target
stage = 0
state = {}


def surface():
    # Sculpt mode evaluates multires as grids, so read the subdivided surface from Object mode.
    obj = bpy.context.sculpt_object
    assert bpy.ops.object.mode_set(mode="OBJECT") == {"FINISHED"}
    try:
        mesh = obj.evaluated_get(bpy.context.evaluated_depsgraph_get()).data
        return tuple(tuple(round(axis, 5) for axis in vertex.co) for vertex in mesh.vertices)
    finally:
        assert bpy.ops.object.mode_set(mode="SCULPT") == {"FINISHED"}


def base():
    return tuple(tuple(round(axis, 5) for axis in vertex.co) for vertex in bpy.context.sculpt_object.data.vertices)


def stroke(area, region):
    assert bpy.ops.view3d.view_axis(type="FRONT") == {"FINISHED"}
    view = area.spaces.active.region_3d
    view.view_location = (0, 0, 0)
    view.view_distance = 5.0
    brush = bpy.context.tool_settings.sculpt.brush
    settings = bpy.context.tool_settings.sculpt.unified_paint_settings
    settings.use_unified_size = settings.use_unified_strength = False
    brush.use_locked_size = "SCENE"
    brush.unprojected_size = 0.3
    brush.strength = 0.8
    cx, cy = region.width / 2, region.height / 2
    points = [dict(name="Draw", mouse=(cx + offset, cy), mouse_event=(cx + offset, cy), location=(0, 0, 0),
                   pressure=1.0, size=1.0, time=step / 60, x_tilt=0, y_tilt=0, is_start=step == 0)
              for step, offset in enumerate(range(-30, 31, 3))]
    assert bpy.ops.sculpt.brush_stroke("EXEC_DEFAULT", True, stroke=points, override_location=True) == {"FINISHED"}


def verify():
    global stage
    try:
        if window.workspace != target or bpy.context.mode != "SCULPT":
            return 0.1
        area = next(area for area in window.screen.areas if area.type == "VIEW_3D")
        region = next(region for region in area.regions if region.type == "WINDOW")
        with bpy.context.temp_override(window=window, area=area, region=region):
            obj = bpy.context.sculpt_object
            status = authoring["sculpt_detail_status"]
            if stage == 0:
                assert status(obj) is None, status(obj)
                for name in ("Smooth", "Draw"):
                    assert bpy.ops.sculpt.worldforge_brush(brush=name) == {"FINISHED"}, name
                    assert bpy.context.tool_settings.sculpt.brush.name == name
                state["base"] = base()
                state["plain"] = surface()
                assert len(state["plain"]) == 8
                assert bpy.ops.ed.undo_push(message="Sculpt test start") == {"FINISHED"}
                assert bpy.ops.object.worldforge_add_sculpt_detail("EXEC_DEFAULT", True) == {"FINISHED"}
                assert obj.modifiers["Sculpt Detail"].total_levels == 4
                assert status(obj) == "Sculpt detail level 4 of 4", status(obj)
                assert not bpy.ops.object.worldforge_add_sculpt_detail.poll()
            elif stage == 1:
                state["detailed"] = surface()
                assert len(state["detailed"]) > 1000, len(state["detailed"])
                stroke(area, region)
            elif stage == 2:
                state["sculpted"] = surface()
                assert state["sculpted"] != state["detailed"], "Draw did not change the detailed surface"
                assert base() == state["base"], "Sculpting changed the base mesh"
                assert bpy.ops.ed.undo() == {"FINISHED"}
            elif stage == 3:
                assert surface() == state["detailed"], "Undo did not restore the surface before the stroke"
                assert bpy.ops.ed.redo() == {"FINISHED"}
            elif stage == 4:
                assert surface() == state["sculpted"], "Redo did not restore the sculpted surface"
                assert bpy.ops.ed.undo() == {"FINISHED"}
                assert bpy.ops.ed.undo() == {"FINISHED"}
            elif stage == 5:
                obj = bpy.context.sculpt_object
                assert not obj.modifiers, "Undo did not remove the added sculpt detail"
                assert surface() == state["plain"] and base() == state["base"]
                assert bpy.ops.ed.redo() == {"FINISHED"}
                assert bpy.ops.ed.redo() == {"FINISHED"}
            else:
                obj = bpy.context.sculpt_object
                assert obj.modifiers["Sculpt Detail"].total_levels == 4
                assert surface() == state["sculpted"] and base() == state["base"]
                obj.modifiers.new("Blocker", "BEVEL")
                assert not bpy.ops.object.worldforge_add_sculpt_detail.poll()
                obj.modifiers.remove(obj.modifiers["Sculpt Detail"])
                assert status(obj) == "Apply or remove modifiers before adding sculpt detail", status(obj)
                (exchange / "result").write_text(
                    "PASS: Draw and Smooth buttons, Add Sculpt Detail, stroke undo and redo, detail undo and redo, "
                    "base mesh preserved, and the unavailable reason.", encoding="utf-8")
                bpy.ops.wm.quit_blender("EXEC_DEFAULT")
                return None
        stage += 1
        return 0.3
    except Exception:
        (exchange / "result").write_text(traceback.format_exc(), encoding="utf-8")
        bpy.ops.wm.quit_blender("EXEC_DEFAULT")
        return None


bpy.app.timers.register(verify, first_interval=0.5)
