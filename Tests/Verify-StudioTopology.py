"""Verify the Studio handoff and optional repair of a legacy box."""

from pathlib import Path
import runpy
import shutil
import sys
import tempfile

import bpy
from bl_operators import worldforge_documents


bridge, fixtures, *legacy_args = (Path(value) for value in sys.argv[sys.argv.index("--") + 1:])


def open_in_studio(exchange):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    sys.argv = ["studio", "--", str(exchange)]
    return runpy.run_path(str(bridge))


with tempfile.TemporaryDirectory(prefix="worldforge-topology-") as directory:
    exchange = Path(directory)
    for filename, vertex_count, edge_count, face_sizes in (
        ("authored-studio-input.glb", 8, 12, [4] * 6),
        ("triangular-studio-input.glb", 8, 13, [3, 3] + [4] * 5),
    ):
        shutil.copyfile(fixtures / filename, exchange / "input.glb")
        session = open_in_studio(exchange)
        mesh = bpy.context.view_layer.objects.active.data
        assert len(mesh.vertices) == vertex_count and len(mesh.edges) == edge_count
        assert sorted(face.loop_total for face in mesh.polygons) == face_sizes
        assert len({tuple(corner.vector) for corner in mesh.uv_layers.active.uv}) > 4
        assert not bpy.ops.mesh.worldforge_restore_box_faces.poll()
        assert not bpy.ops.mesh.worldforge_setup_uvs.poll()
        bpy.types.TOPBAR_MT_file.remove(session["draw_publish"])
        bpy.utils.unregister_class(session["WM_OT_publish_to_worldforge"])
        bpy.utils.unregister_class(session["MESH_OT_worldforge_restore_box_faces"])
        bpy.utils.unregister_class(session["MESH_OT_worldforge_setup_uvs"])
        print(f"PASS: {filename} retains authored faces, edges, and UVs")

    if legacy_args:
        shutil.copyfile(legacy_args[0], exchange / "source.wfasset")
        open_in_studio(exchange)
        obj = bpy.context.view_layer.objects.active
        assert len(obj.data.polygons) == 12
        assert bpy.ops.object.mode_set(mode="EDIT") == {"FINISHED"}
        assert bpy.ops.mesh.worldforge_restore_box_faces() == {"FINISHED"}
        assert obj.mode == "EDIT"
        assert bpy.ops.object.mode_set(mode="OBJECT") == {"FINISHED"}
        assert len(obj.data.vertices) == 8 and len(obj.data.edges) == 12 and len(obj.data.polygons) == 6
        assert all(face.loop_total == 4 for face in obj.data.polygons)
        assert len({tuple(corner.vector) for corner in obj.data.uv_layers.active.uv}) > 4
        assert bpy.ops.wm.publish_to_worldforge() == {"FINISHED"}
        published = exchange / (exchange / "ready").read_text(encoding="ascii")
        worldforge_documents.open_asset(published / "source.wfasset", remember=False, working_copy=True)
        reopened = bpy.context.view_layer.objects.active.data
        assert len(reopened.edges) == 12 and [face.loop_total for face in reopened.polygons] == [4] * 6
        print("PASS: the saved QA cube repairs and republishes as six connected quads with its UVs intact")
    else:
        open_in_studio(exchange)

    bpy.ops.mesh.primitive_cube_add(calc_uvs=False)
    obj = bpy.context.active_object
    mesh = obj.data
    bpy.ops.mesh.primitive_plane_add(calc_uvs=False)
    other = bpy.context.active_object
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    mesh.vertices[0].select = False
    selection = [vertex.select for vertex in mesh.vertices]
    assert not mesh.uv_layers
    assert bpy.ops.mesh.worldforge_setup_uvs() == {"FINISHED"}
    assert other.select_get() and not other.data.uv_layers
    assert [vertex.select for vertex in mesh.vertices] == selection
    assert not bpy.ops.mesh.worldforge_setup_uvs.poll()

    for uv in mesh.uv_layers.active.uv:
        uv.vector = (0, 0)
    assert bpy.ops.object.mode_set(mode="TEXTURE_PAINT") == {"FINISHED"}
    assert bpy.ops.mesh.worldforge_setup_uvs() == {"FINISHED"}
    assert bpy.context.mode == "PAINT_TEXTURE" and len(mesh.uv_layers) == 1
    assert not bpy.ops.mesh.worldforge_setup_uvs.poll()
    assert bpy.ops.wm.publish_to_worldforge() == {"FINISHED"}
    published = exchange / (exchange / "ready").read_text(encoding="ascii")
    worldforge_documents.open_asset(published / "source.wfasset", remember=False, working_copy=True)
    reopened = bpy.context.view_layer.objects.active.data
    assert len({tuple(corner.vector) for corner in reopened.uv_layers.active.uv}) > 4
    print("PASS: UV setup handles missing and collapsed layouts, preserves selection and paint mode, and publishes its UVs")
