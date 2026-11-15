"""Open every bundled Essentials brush and node library in the shipped Studio."""

from pathlib import Path
import shutil
import tempfile

import bpy
from bl_operators.worldforge_asset import write_package


root = Path(bpy.utils.system_resource("DATAFILES")) / "assets"
collections = {"brushes": "brushes", "nodes": "node_groups"}
for folder, collection in collections.items():
    assert not list((root / folder).glob("*.wflib")), folder
    libraries = sorted((root / folder).glob("*.blend"))
    assert len(libraries) == {"brushes": 9, "nodes": 6}[folder], (folder, libraries)
    for library in libraries:
        with bpy.data.libraries.load(str(library)) as (available, requested):
            names = getattr(available, collection)
            assert names, library
            setattr(requested, collection, names)
        loaded = getattr(requested, collection)
        assert len(loaded) == len(names) and all(item is not None for item in loaded), library
        print(f"PASS: {library.name} ({len(loaded)} {collection})")

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.mesh.primitive_cube_add()
assert bpy.ops.ed.undo_push(message="Sculpting test setup") == {"FINISHED"}
assert bpy.ops.object.mode_set(mode="SCULPT") == {"FINISHED"}
assert bpy.context.tool_settings.sculpt.brush.name == "Draw"
for extension, brush in ((".blend", "Draw"), (".wflib", "Grab")):
    identifier = f"brushes/essentials_brushes-mesh_sculpt{extension}/Brush/{brush}"
    assert bpy.ops.brush.asset_activate(
        asset_library_type="ESSENTIALS", relative_asset_identifier=identifier
    ) == {"FINISHED"}, identifier
    assert bpy.context.tool_settings.sculpt.brush.name == brush, identifier
    print(f"PASS: Sculpting activates {brush} through the {extension} asset reference")


obj = bpy.context.sculpt_object
base_positions = tuple(tuple(vertex.co) for vertex in obj.data.vertices)
assert bpy.ops.brush.asset_activate(
    asset_library_type="ESSENTIALS",
    relative_asset_identifier="brushes/essentials_brushes-mesh_sculpt.blend/Brush/Draw",
) == {"FINISHED"}
brush = bpy.context.tool_settings.sculpt.brush
settings = bpy.context.tool_settings.sculpt.unified_paint_settings
settings.use_unified_size = settings.use_unified_strength = False
brush.use_locked_size = "SCENE"
brush.unprojected_size = 0.3
brush.strength = 0.8
area = next(area for area in bpy.context.screen.areas if area.type == "VIEW_3D")
region = next(region for region in area.regions if region.type == "WINDOW")


def draw_stroke():
    with bpy.context.temp_override(area=area, region=region):
        assert bpy.ops.view3d.view_axis(type="FRONT") == {"FINISHED"}
        view = area.spaces.active.region_3d
        view.view_location = (0, 0, 0)
        view.view_distance = 5.0
        cx, cy = region.width / 2, region.height / 2
        stroke = [dict(
            name="Draw", mouse=(cx + offset, cy), mouse_event=(cx + offset, cy),
            location=(0, 0, 0), pressure=1.0, size=1.0, time=step / 60,
            x_tilt=0, y_tilt=0, is_start=(step == 0),
        ) for step, offset in enumerate(range(-30, 31, 3))]
        assert bpy.ops.sculpt.brush_stroke(stroke=stroke, override_location=True) == {"FINISHED"}


def surface_positions():
    assert bpy.ops.object.mode_set(mode="OBJECT") == {"FINISHED"}
    mesh = obj.evaluated_get(bpy.context.evaluated_depsgraph_get()).data
    positions = tuple(tuple(vertex.co) for vertex in mesh.vertices)
    assert bpy.ops.object.mode_set(mode="SCULPT") == {"FINISHED"}
    return positions


draw_stroke()
assert base_positions == tuple(tuple(vertex.co) for vertex in obj.data.vertices)
assert bpy.ops.object.worldforge_add_sculpt_detail() == {"FINISHED"}
assert obj.modifiers[0].total_levels == obj.modifiers[0].sculpt_levels == 4
assert not bpy.ops.object.worldforge_add_sculpt_detail.poll()
before = surface_positions()
assert len(before) == 1538, len(before)
draw_stroke()
after = surface_positions()
assert len(after) == len(before) and any(a != b for a, b in zip(after, before))
assert base_positions == tuple(tuple(vertex.co) for vertex in obj.data.vertices)
print("PASS: Draw deforms the detailed cube surface while its original eight vertices are preserved")

with tempfile.TemporaryDirectory(prefix="studio-sculpt-detail-") as directory:
    asset = Path(directory) / "sculpted.wfasset"
    assert bpy.ops.wm.worldforge_save_asset(filepath=str(asset)) == {"FINISHED"}
    bpy.ops.wm.read_factory_settings(use_empty=True)
    assert bpy.ops.wm.worldforge_open_asset(filepath=str(asset)) == {"FINISHED"}
    obj = bpy.data.objects["Cube"]
    assert obj.modifiers["Sculpt Detail"].total_levels == 4
    assert tuple(tuple(vertex.co) for vertex in obj.data.vertices) == base_positions
    assert bpy.ops.object.mode_set(mode="OBJECT") == {"FINISHED"}
    reopened = obj.evaluated_get(bpy.context.evaluated_depsgraph_get()).data
    assert tuple(tuple(vertex.co) for vertex in reopened.vertices) == after
    bpy.ops.wm.read_factory_settings(use_empty=True)
    print("PASS: The .wfasset round trip preserves the base mesh and the sculpted surface")


# Documents saved during the .wflib migration still need to resolve their linked nodes.
with tempfile.TemporaryDirectory(prefix="studio-legacy-library-") as directory:
    directory = Path(directory)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    source = root / "nodes" / "geometry_nodes_essentials.blend"
    old = directory / "old-install" / "5.2" / "datafiles" / "assets" / "nodes" / source.with_suffix(".wflib").name
    old.parent.mkdir(parents=True)
    shutil.copyfile(source, old)
    with bpy.data.libraries.load(str(old), link=True) as (available, requested):
        assert "Smooth by Angle" in available.node_groups
        requested.node_groups = ["Smooth by Angle"]
    bpy.ops.mesh.primitive_cube_add()
    modifier = bpy.context.object.modifiers.new("Legacy Smooth", "NODES")
    modifier.node_group = requested.node_groups[0]
    payload = directory / "legacy.blend"
    assert bpy.ops.wm.save_as_mainfile(
        filepath=str(payload), copy=True, relative_remap=False
    ) == {"FINISHED"}
    asset = directory / "legacy.wfasset"
    write_package(asset, payload, bpy.app.version_string)
    old.unlink()
    from bl_operators import worldforge_documents
    worldforge_documents.snapshot_asset(directory / "republished.wfasset")
    assert Path(modifier.node_group.library.filepath).resolve() == source.resolve()
    bpy.ops.wm.read_factory_settings(use_empty=True)
    assert bpy.ops.wm.worldforge_open_asset(filepath=str(directory / "republished.wfasset")) == {"FINISHED"}
    assert not bpy.data.objects["Cube"].modifiers["Legacy Smooth"].node_group.is_missing
    bpy.ops.wm.read_factory_settings(use_empty=True)
    assert bpy.ops.wm.worldforge_open_asset(filepath=str(asset)) == {"FINISHED"}
    group = bpy.data.objects["Cube"].modifiers["Legacy Smooth"].node_group
    assert group.name == "Smooth by Angle"
    assert len(group.nodes) > 0 and not group.is_missing
    assert Path(group.library.filepath).resolve() == source.resolve(), group.library.filepath
    assert bpy.ops.wm.worldforge_save_asset(filepath=str(directory / "migrated.wfasset")) == {"FINISHED"}
    bpy.ops.wm.read_factory_settings(use_empty=True)
    print("PASS: a .wflib library link reopened and saved with its restored .blend replacement")
