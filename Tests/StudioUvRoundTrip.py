# SPDX-License-Identifier: GPL-2.0-or-later

"""Exercise the actual Studio import and publish commands with two named UV layers."""

from pathlib import Path
import sys

import bpy
from bl_operators import worldforge_documents


exchange = Path(sys.argv[sys.argv.index("--") + 1]).resolve()
obj = bpy.context.object
layers = obj.data.uv_layers
assert list(layers.keys()) == ["UV Map", "Lightmap"]
assert len(obj.data.polygons) == 6
assert layers[0].active_render
layout = layers["Lightmap"]
assert all(0.10 <= elem.vector.x <= 0.51 for elem in layout.uv)
for elem in layout.uv:
    elem.vector = (0.25 + elem.vector.x * 0.5, 0.15 + elem.vector.y * 0.6)
layers.active_index = 1
assert layers[0].active_render
assert bpy.ops.wm.publish_to_worldforge() == {"FINISHED"}
first_revision = (exchange / "ready").read_text(encoding="ascii")
(exchange / "with-lightmap").write_text(first_revision, encoding="ascii")

# Remove the old lightmap and change topology to exercise the native stale layout diagnostic.
layers.remove(layers["Lightmap"])
bpy.ops.object.mode_set(mode="EDIT")
bpy.ops.mesh.select_all(action="SELECT")
bpy.ops.mesh.subdivide(number_cuts=1)
bpy.ops.object.mode_set(mode="OBJECT")
assert bpy.ops.wm.publish_to_worldforge() == {"FINISHED"}

worldforge_documents.open_asset(exchange / first_revision / "source.wfasset", remember=False, working_copy=True)
visible = bpy.context.object
assert list(visible.data.uv_layers.keys()) == ["UV Map", "Lightmap"]
assert visible.data.uv_layers["UV Map"].active_render
assert "UV Trim" in visible.data.materials.keys()
