"""Exercise the workspace bridge in an interactive Studio process."""

from pathlib import Path
import runpy
import sys
import time
import traceback

import bpy


exchange_arg, bridge_arg, *options = sys.argv[sys.argv.index("--") + 1:]
exchange, bridge = Path(exchange_arg), Path(bridge_arg)
edited_starter = "--edited-starter" in options
request_file = exchange / "workspace"
result_file = exchange / "result"
targets = iter((
    ("Sculpting", "Sculpting"),
    ("Modeling", "Modeling"),
    ("Sculpting", "Sculpting"),
    ("UV Editing", "UV Editing"),
    ("Texture", "Texture Paint"),
    ("Material", "Shading"),
    ("Rigging", "Rigging"),
    ("Animation", "Animation"),
    ("Modeling", "Modeling"),
))
obj = bpy.data.objects["Cube"]
completed = []
deadline = time.monotonic() + 45
target = next(targets)
request_file.write_text(target[0], encoding="utf-8")
runpy.run_path(str(bridge))
if edited_starter:
    obj.data.vertices[0].co.x += 0.25
base_positions = [tuple(vertex.co) for vertex in obj.data.vertices]


def verify():
    global target
    try:
        window = next(iter(bpy.context.window_manager.windows))
        ready = exchange / "workspace-ready"
        if window.workspace.name != target[1] or not ready.exists() or ready.read_bytes() != request_file.read_bytes():
            assert time.monotonic() < deadline, (target, window.workspace.name)
            return 0.15
        assert bpy.data.objects["Cube"] == obj
        assert [tuple(vertex.co) for vertex in obj.data.vertices] == base_positions
        if target[0] == "Sculpting":
            assert obj.mode == "SCULPT"
            assert bpy.context.tool_settings.sculpt.brush.name == "Draw"
            detail = [modifier for modifier in obj.modifiers if modifier.type == 'MULTIRES']
            if edited_starter:
                assert not detail
            else:
                assert len(detail) == 1 and detail[0].sculpt_levels == 4
        if target[0] == "Modeling" and "Unsaved workspace edit" not in obj.modifiers:
            obj.modifiers.new("Unsaved workspace edit", "BEVEL").segments = 5
        if completed and target[0] != "Sculpting":
            assert obj.modifiers["Unsaved workspace edit"].segments == 5
        completed.append(target[0])
        target = next(targets, None)
        if target is not None:
            request_file.write_text(target[0], encoding="utf-8")
            return 0.15
        result_file.write_text("PASS: " + " → ".join(completed) +
                               f" (edited starter: {edited_starter})", encoding="utf-8")
    except Exception:
        result_file.write_text(traceback.format_exc(), encoding="utf-8")
    bpy.ops.wm.quit_blender("EXEC_DEFAULT")
    return None


bpy.app.timers.register(verify, first_interval=0.25)
