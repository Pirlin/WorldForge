"""Exercise a mesh edit, modifier evaluation, save, and reopen in Blender."""

import argparse
import json
import sys
from pathlib import Path

import bmesh
import bpy


def check(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--build-hash", default="e13b728c6880")
    parser.add_argument("--studio", action="store_true")
    args = parser.parse_args(sys.argv[sys.argv.index("--") + 1:])
    actual_hash = bpy.app.build_hash.decode("ascii")
    check(actual_hash == args.build_hash, f"Unexpected build hash {actual_hash}")
    args.output.mkdir(parents=True, exist_ok=True)
    fixture = args.output / "worldforge-baseline.blend"

    studio_details = {}
    if args.studio:
        check(Path(bpy.app.binary_path).name == "WorldForgeStudio.exe",
              "Studio fixture ran using an unexpected executable")
        config_path = Path(bpy.utils.user_resource("CONFIG"))
        check(config_path.parts[-4:-1] == ("WorldForge", "Studio", "5.2"),
              f"Unexpected Studio preferences path {config_path}")
        theme = bpy.context.preferences.themes[0]
        selected = tuple(theme.user_interface.wcol_regular.inner_sel)
        check(all(abs(value - expected / 255) < 0.01
                  for value, expected in zip(selected[:3], (120, 68, 207))),
              f"Unexpected Studio selection color {selected}")
        workspaces = sorted(workspace.name for workspace in bpy.data.workspaces)
        check({"Layout", "Modeling", "Sculpting", "UV Editing", "Texture Paint",
               "Shading", "Animation"}.issubset(workspaces),
              f"Missing Studio workspaces {workspaces}")
        studio_details = {"config": str(config_path), "workspaces": workspaces}

    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.mesh.primitive_cube_add()
    cube = bpy.context.active_object
    cube.name = "WorldForge Baseline Cube"
    bpy.ops.object.mode_set(mode="EDIT")
    mesh = bmesh.from_edit_mesh(cube.data)
    for vertex in mesh.verts:
        vertex.co.z += 0.375
    bmesh.update_edit_mesh(cube.data)
    bpy.ops.object.mode_set(mode="OBJECT")
    check(len(cube.data.vertices) == 8, "Cube vertex count changed unexpectedly")
    check(abs(max(v.co.z for v in cube.data.vertices) - 1.375) < 1e-6,
          "Edit Mode change did not reach the mesh")

    bevel = cube.modifiers.new(name="Baseline Bevel", type="BEVEL")
    bevel.width = 0.12
    bevel.segments = 3
    bpy.context.view_layer.update()
    polygons = len(cube.evaluated_get(bpy.context.evaluated_depsgraph_get()).data.polygons)
    check(polygons > 6, "Bevel did not increase the evaluated polygon count")

    bpy.ops.wm.save_as_mainfile(filepath=str(fixture))
    bpy.ops.wm.open_mainfile(filepath=str(fixture), load_ui=False, use_scripts=False)
    cube = bpy.data.objects["WorldForge Baseline Cube"]
    check(abs(max(v.co.z for v in cube.data.vertices) - 1.375) < 1e-6,
          "Reopened mesh lost its Edit Mode change")
    check(cube.modifiers["Baseline Bevel"].segments == 3, "Reopened mesh lost its editable bevel")
    reopened_polygons = len(cube.evaluated_get(bpy.context.evaluated_depsgraph_get()).data.polygons)
    check(reopened_polygons == polygons, "Reopened bevel evaluated to a different mesh")

    result = {
        "status": "PASS",
        "binary": bpy.app.binary_path,
        "version": bpy.app.version_string,
        "commit": actual_hash,
        "fixture": str(fixture),
        "edited_max_z": 1.375,
        "base_polygons": len(cube.data.polygons),
        "evaluated_polygons": reopened_polygons,
        **studio_details,
    }
    (args.output / "baseline-result.json").write_text(
        json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
