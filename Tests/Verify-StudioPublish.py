"""Exercise the Studio publishing bridge against an editable mesh."""

import argparse
import math
from pathlib import Path
import runpy
import shutil
import sys
import tempfile

import bpy
import bl_operators


def sculpt_and_texture(obj, exchange):
    """Stage a sculpted barrier with a painted base-color image."""
    assert bpy.ops.ed.undo_push(message="Barrier import") == {"FINISHED"}
    assert bpy.ops.object.mode_set(mode="SCULPT") == {"FINISHED"}
    sculpt = bpy.context.tool_settings.sculpt
    assert sculpt.brush.name == "Draw"
    sculpt.unified_paint_settings.use_unified_size = False
    sculpt.brush.use_locked_size = "SCENE"
    sculpt.brush.unprojected_size = 0.35
    assert bpy.ops.object.worldforge_add_sculpt_detail() == {"FINISHED"}
    area = next(item for item in bpy.context.screen.areas if item.type == "VIEW_3D")
    region = next(item for item in area.regions if item.type == "WINDOW")
    with bpy.context.temp_override(area=area, region=region):
        assert bpy.ops.view3d.view_axis(type="FRONT") == {"FINISHED"}
        area.spaces.active.region_3d.view_location = (0, 0, 0)
        area.spaces.active.region_3d.view_distance = 5.0
        cx, cy = region.width / 2, region.height / 2
        stroke = [dict(name="Draw", mouse=(cx + x, cy), mouse_event=(cx + x, cy),
                       location=(0, 0, 0), pressure=1, size=1, time=step / 60,
                       x_tilt=0, y_tilt=0, is_start=step == 0)
                  for step, x in enumerate(range(-30, 31, 3))]
        assert bpy.ops.sculpt.brush_stroke(stroke=stroke, override_location=True) == {"FINISHED"}
    assert bpy.ops.object.mode_set(mode="OBJECT") == {"FINISHED"}
    surface = obj.evaluated_get(bpy.context.evaluated_depsgraph_get()).data
    assert any(abs(vertex.co.y) > 1.01 for vertex in surface.vertices)

    material = obj.data.materials[0]
    material.use_nodes = True
    shader = next(node for node in material.node_tree.nodes if node.type == "BSDF_PRINCIPLED")
    shader.inputs["Base Color"].default_value = (1, 1, 1, 1)
    image = bpy.data.images.new("Barrier Paint", width=16, height=16, alpha=True)
    image.pixels = [value for y in range(16) for x in range(16)
                    for value in ((1.0, 0.72, 0.05, 1.0) if 3 <= x < 11 else (0.1, 0.1, 0.1, 1.0))]
    image.filepath_raw = str(exchange / "barrier-paint.png")
    image.file_format = "PNG"
    image.save()
    texture = material.node_tree.nodes.new("ShaderNodeTexImage")
    texture.image = image
    material.node_tree.links.new(texture.outputs["Color"], shader.inputs["Base Color"])
    material.node_tree.nodes.active = texture
    return image


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bridge", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--modules", type=Path)
    parser.add_argument("--barrier", action="store_true")
    options = parser.parse_args(sys.argv[sys.argv.index("--") + 1:])
    if options.modules:
        bl_operators.__path__.insert(0, str(options.modules.resolve()))
    from bl_operators import worldforge_documents
    from bl_operators.worldforge_asset import read_package

    options.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="studio-publish-qa-") as temporary:
        exchange = Path(temporary)
        bpy.ops.wm.read_factory_settings(use_empty=True)
        bpy.ops.mesh.primitive_cube_add()
        cube = bpy.context.object
        for name, color in (("Purple", (0.6, 0.2, 0.8, 1.0)), ("Gold", (0.8, 0.6, 0.1, 1.0))):
            material = bpy.data.materials.new(name)
            material.diffuse_color = (0.45, 0.45, 0.45, 1.0) if options.barrier else color
            cube.data.materials.append(material)
        cube.data.polygons[1].material_index = 1
        bpy.ops.export_scene.gltf(filepath=str(exchange / "input.glb"), export_format="GLB")

        sys.argv = ["studio", "--", str(exchange)]
        session = runpy.run_path(str(options.bridge))
        obj = bpy.context.active_object
        assert len(obj.data.materials) == 2
        bpy.ops.object.mode_set(mode="EDIT")
        bpy.ops.mesh.select_all(action="SELECT")
        if options.barrier:
            bpy.ops.transform.resize(value=(1.8, 1.0, 0.65))
            bpy.ops.mesh.remove_doubles()
            bpy.ops.uv.smart_project(island_margin=0.05)
        else:
            bpy.ops.uv.unwrap(method="ANGLE_BASED", margin=0.05)
        bpy.ops.object.mode_set(mode="OBJECT")
        uv_map = obj.data.uv_layers.active
        assert uv_map is not None
        authored_uvs = [tuple(loop.uv) for loop in uv_map.data]
        assert len(set(authored_uvs)) > 4 and all(
            math.isfinite(value) and -0.0001 <= value <= 1.0001
            for uv in authored_uvs for value in uv
        )
        if options.barrier:
            paint = sculpt_and_texture(obj, exchange)
        else:
            obj.modifiers.new("Publish Bevel", "BEVEL").segments = 3

        def publish():
            assert bpy.ops.wm.publish_to_worldforge() == {"FINISHED"}
            ready = exchange / (exchange / "ready").read_text(encoding="ascii")
            assert (ready / "mesh.glb").is_file()
            return ready, read_package(ready / "source.wfasset")

        _, first = publish()
        second_path, second = publish()
        assert second["asset_id"] == first["asset_id"]
        assert second["revision"] == first["revision"] + 1
        shutil.copyfile(second_path / "source.wfasset", exchange / "source.wfasset")
        if options.barrier:
            Path(paint.filepath_raw).unlink()
        bpy.utils.unregister_class(session["WM_OT_publish_to_worldforge"])
        bpy.utils.unregister_class(session["MESH_OT_worldforge_restore_box_faces"])
        bpy.utils.unregister_class(session["MESH_OT_worldforge_setup_uvs"])
        bpy.types.TOPBAR_MT_file.remove(session["draw_publish"])
        runpy.run_path(str(options.bridge))
        if options.barrier:
            assert bpy.context.active_object.modifiers["Sculpt Detail"].total_levels == 4
            restored = bpy.data.images["Barrier Paint"]
            assert restored.packed_file is not None and max(restored.pixels) > 0
        else:
            assert bpy.context.active_object.modifiers["Publish Bevel"].segments == 3
        assert len(bpy.context.active_object.data.materials) == 2
        assert [tuple(loop.uv) for loop in bpy.context.active_object.data.uv_layers.active.data] == authored_uvs
        assert worldforge_documents.active_path() is None
        staged_source = (exchange / "source.wfasset").read_bytes()
        saved = worldforge_documents.save_asset(exchange / "saved-edit.wfasset")
        assert saved["asset_id"] == first["asset_id"]
        assert (exchange / "source.wfasset").read_bytes() == staged_source
        third_path, third = publish()
        assert third["asset_id"] == first["asset_id"] and third["revision"] == second["revision"] + 1
        if options.barrier:
            staged = (exchange / "ready").read_bytes()
            missing = exchange / "missing-texture.png"
            missing.write_bytes(bpy.data.images["Barrier Paint"].packed_file.data)
            texture = bpy.data.images.load(str(missing))
            missing.unlink()
            material = bpy.context.active_object.data.materials[0]
            node = next(node for node in material.node_tree.nodes if node.type == "TEX_IMAGE")
            node.image = texture
            try:
                result = bpy.ops.wm.publish_to_worldforge()
            except RuntimeError as error:
                assert "missing-texture.png" in str(error), error
            else:
                assert result == {"CANCELLED"}
            assert (exchange / "ready").read_bytes() == staged
        for name in ("source.wfasset", "mesh.glb"):
            shutil.copyfile(third_path / name, options.output / name)
        bpy.ops.wm.read_factory_settings(use_empty=True)
        assert bpy.ops.import_scene.gltf(filepath=str(options.output / "mesh.glb")) == {"FINISHED"}
        evaluated = bpy.context.active_object.data
        assert len(evaluated.polygons) > 6
        assert {face.material_index for face in evaluated.polygons} == {0, 1}
        assert len({tuple(loop.uv) for loop in evaluated.uv_layers.active.data}) > 4
        if options.barrier:
            assert any(node.type == "TEX_IMAGE" and node.image for material in evaluated.materials
                       for node in material.node_tree.nodes)
    detail = "sculpted barrier and embedded base color" if options.barrier else "UV unwrap and two-material bevel"
    print(f"PASS: {detail} reopen, evaluated GLB and stable asset identity")


if __name__ == "__main__":
    main()
