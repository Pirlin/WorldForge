"""Paint on the barrier in Studio's Texture Paint workspace and reopen its published source."""

import runpy
import shutil
import sys
import traceback
from pathlib import Path

import bpy
from bl_operators import worldforge_documents


exchange, bridge, fixture = (Path(arg) for arg in sys.argv[sys.argv.index("--") + 1:])
shutil.copyfile(fixture, exchange / "source.wfasset")
authoring = runpy.run_path(str(bridge))
window = next(iter(bpy.context.window_manager.windows))
obj = window.view_layer.objects.active
assert obj.modifiers["Sculpt Detail"].total_levels == 4
material = obj.data.materials[0]
texture = next(node for node in material.node_tree.nodes if node.type == "TEX_IMAGE")
image = bpy.data.images.new("Live Barrier Paint", width=128, height=128, alpha=True)
image.generated_color = (0.12, 0.12, 0.12, 1)
texture.image = image
material.node_tree.nodes.active = texture
target = bpy.data.workspaces["Texture Paint"]
window.workspace = target
stage = 0


def paint_and_publish():
    global stage
    try:
        if window.workspace != target:
            return 0.1
        area = next(area for area in window.screen.areas if area.type == "IMAGE_EDITOR")
        region = next(region for region in area.regions if region.type == "WINDOW")
        if stage == 0:
            area.spaces.active.image = image
            area.spaces.active.mode = "PAINT"
            stage = 1
            return 0.3
        with bpy.context.temp_override(window=window, area=area, region=region):
            if stage == 1:
                assert bpy.ops.image.view_all(fit_view=True) == {"FINISHED"}
                stage = 2
                return 0.3
            paint = bpy.context.tool_settings.image_paint
            paint_material, paint_image = authoring["paint_target"](bpy.context)
            assert paint_image == image and (paint.mode == "IMAGE" or paint_material == material), (
                paint.mode, paint_material, paint_image, material, image, obj.active_material)
            paint.unified_paint_settings.use_unified_color = False
            paint.unified_paint_settings.use_unified_size = False
            paint.unified_paint_settings.use_unified_strength = False
            paint.brush.color = (0.95, 0.6, 0.02)
            paint.brush.strength = 1
            paint.brush.size = 200
            start = region.view2d.view_to_region(0.2, 0.5, clip=False)
            end = region.view2d.view_to_region(0.8, 0.5, clip=False)
            assert 0 <= start[0] < end[0] < region.width and 0 <= start[1] < region.height
            stroke = []
            for step in range(20):
                pos = (start[0] + (end[0] - start[0]) * step / 19, start[1])
                stroke.append(dict(name="Texture Paint", mouse=pos, mouse_event=pos,
                                   location=(0, 0, 0), pressure=1, size=paint.brush.size, time=step / 60,
                                   x_tilt=0, y_tilt=0, is_start=step == 0))
            before = list(image.pixels)
            assert bpy.ops.paint.image_paint(stroke=stroke) == {"FINISHED"}
            after = list(image.pixels)
            assert before != after and image.is_dirty
            mode = obj.mode
            assert bpy.ops.wm.publish_to_worldforge() == {"FINISHED"}
            assert obj.mode == mode
        published = exchange / (exchange / "ready").read_text(encoding="ascii")
        assert (published / "mesh.glb").is_file()
        worldforge_documents.open_asset(published / "source.wfasset", remember=False, working_copy=True)
        restored = bpy.data.images["Live Barrier Paint"]
        assert restored.packed_file is not None
        reopened = list(restored.pixels)
        assert len(reopened) == len(after) and max(abs(a - b) for a, b in zip(reopened, after)) <= 2 / 255
        (exchange / "result").write_text("PASS: brush stroke published and reopened with painted pixels intact", encoding="utf-8")
    except Exception:
        (exchange / "result").write_text(traceback.format_exc(), encoding="utf-8")
    bpy.ops.wm.quit_blender("EXEC_DEFAULT")
    return None


bpy.app.timers.register(paint_and_publish, first_interval=0.5)
