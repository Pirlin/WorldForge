"""Exercise the Modeling header's native bevel and transform-pivot operations."""

from pathlib import Path
import runpy
import sys
import traceback

import bmesh
import bpy


exchange, bridge = (Path(value) for value in sys.argv[sys.argv.index("--") + 1:][:2])
runpy.run_path(str(bridge))
window = bpy.context.window
window.workspace = bpy.data.workspaces["Modeling"]
stage = 0
baseline = None
beveled = None


def snapshot():
    mesh = bmesh.from_edit_mesh(bpy.context.edit_object.data)
    return sorted(tuple(round(axis, 5) for axis in vertex.co) for vertex in mesh.verts), len(mesh.faces)


def verify():
    global stage, baseline, beveled
    try:
        if window.workspace.name != "Modeling":
            return 0.1
        area = next(area for area in window.screen.areas if area.type == "VIEW_3D")
        region = next(region for region in area.regions if region.type == "WINDOW")
        x, y = region.x + region.width // 2, region.y + region.height // 2
        with bpy.context.temp_override(window=window, area=area, region=region):
            if stage == 0:
                if bpy.context.mode != "EDIT_MESH":
                    bpy.ops.object.mode_set(mode="EDIT")
                bpy.context.tool_settings.mesh_select_mode = (False, True, False)
                bpy.ops.mesh.select_all(action="SELECT")
                baseline = snapshot()
                bpy.ops.ed.undo_push(message="Selected cube edges")
                window.event_simulate(type="MOUSEMOVE", value="NOTHING", x=x, y=y)
            elif stage == 1:
                assert bpy.ops.mesh.bevel("INVOKE_DEFAULT", affect="EDGES") == {"RUNNING_MODAL"}
                window.event_simulate(type="MOUSEMOVE", value="NOTHING", x=x+32, y=y)
            elif stage == 2:
                assert snapshot() != baseline, "Modal bevel did not preview"
                window.event_simulate(type="ESC", value="PRESS", x=x+32, y=y)
                window.event_simulate(type="ESC", value="RELEASE", x=x+32, y=y)
            elif stage == 3:
                assert snapshot() == baseline, "Cancel changed the mesh"
                # Pass undo=True so the scripted bevel records an undo step like the header button.
                assert bpy.ops.mesh.bevel("EXEC_DEFAULT", True, affect="EDGES", offset=0.15,
                                          segments=2) == {"FINISHED"}
                beveled = snapshot()
                assert beveled[1] > baseline[1]
                assert bpy.ops.ed.undo() == {"FINISHED"}
            elif stage == 4:
                assert snapshot() == baseline
                assert bpy.ops.ed.redo() == {"FINISHED"}
            else:
                actual_bevel = snapshot()
                assert actual_bevel == beveled, (actual_bevel, beveled)
                bpy.ops.mesh.select_all(action="SELECT")
                bpy.context.scene.cursor.location = (3, 0, 0)
                bpy.context.tool_settings.transform_pivot_point = "CURSOR"
                assert bpy.ops.transform.resize(value=(2, 2, 2)) == {"FINISHED"}
                expected = sorted(tuple(round(2 * value - offset, 4) for value, offset in zip(vertex, (3, 0, 0)))
                                  for vertex in beveled[0])
                actual = [tuple(round(value, 4) for value in vertex) for vertex in snapshot()[0]]
                assert all(abs(a - b) < 0.0002 for point, target in zip(actual, expected, strict=True)
                           for a, b in zip(point, target, strict=True))
                (exchange / "result").write_text("PASS: bevel preview, cancel, apply, undo, redo, and cursor pivot.",
                                                encoding="utf-8")
                bpy.ops.wm.quit_blender("EXEC_DEFAULT")
                return None
        stage += 1
        return 0.2
    except Exception:
        (exchange / "result").write_text(traceback.format_exc(), encoding="utf-8")
        bpy.ops.wm.quit_blender("EXEC_DEFAULT")
        return None


bpy.app.timers.register(verify, first_interval=0.2)
