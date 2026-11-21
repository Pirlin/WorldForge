# SPDX-License-Identifier: GPL-2.0-or-later

"""Verify a sculpt's reduced GLB and editable source with the installed Studio."""

import json
from pathlib import Path
import struct
import sys

import bpy
from bl_operators import worldforge_documents


exchange = Path(sys.argv[sys.argv.index("--") + 1]).resolve()
bpy.ops.mesh.primitive_cube_add()
sculpt = bpy.context.object
sculpt.name = "Workbench Sculpt"
modifier = sculpt.modifiers.new("Multires", "MULTIRES")
for _ in range(4):
    bpy.ops.object.multires_subdivide(modifier=modifier.name)
modifier.levels = modifier.sculpt_levels = modifier.render_levels = 4
sculpt.scale = (0.55, 1.0, 0.85)

budget = 600
engine = bpy.context.scene.render.engine
assert bpy.ops.wm.publish_to_worldforge(triangle_budget=budget, bake_detail=True) == {"FINISHED"}
assert bpy.context.view_layer.objects.active == sculpt
assert bpy.context.scene.render.engine == engine
assert [item.type for item in sculpt.modifiers] == ["MULTIRES"]
assert bpy.data.objects.get(sculpt.name + " Game Mesh") is None

revision = exchange / (exchange / "ready").read_text(encoding="ascii")
binary = (revision / "mesh.glb").read_bytes()
size = struct.unpack_from("<I", binary, 12)[0]
glb = json.loads(binary[20:20 + size])
parts = [part for mesh in glb["meshes"] for part in mesh["primitives"]]
triangles = sum(glb["accessors"][part["indices"]]["count"] // 3 for part in parts)
assert 0 < triangles <= budget, triangles
assert all("NORMAL" in part["attributes"] and "TEXCOORD_0" in part["attributes"] for part in parts)
assert any("normalTexture" in material for material in glb["materials"])

worldforge_documents.open_asset(revision / "source.wfasset", remember=False, working_copy=True)
sculpt = bpy.data.objects["Workbench Sculpt"]
assert len(sculpt.data.polygons) == 6
assert sculpt.modifiers[0].type == "MULTIRES" and sculpt.modifiers[0].levels == 4
report = {
    "exportedTriangles": triangles,
    "sourceBaseFaces": len(sculpt.data.polygons),
    "sourceMultiresLevels": sculpt.modifiers[0].levels,
    "hasUvAndNormals": True,
    "hasBakedNormalMap": True,
}
(exchange / "result.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
