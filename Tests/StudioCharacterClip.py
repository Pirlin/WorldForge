# SPDX-License-Identifier: GPL-2.0-or-later

"""Publish a weighted two-bone character with a visible looping bend."""

from pathlib import Path
import sys

import bpy
from bl_operators import worldforge_documents


exchange = Path(sys.argv[sys.argv.index("--") + 1]).resolve()
bones = bpy.data.armatures.new("Two Bone Rig")
rig = bpy.data.objects.new("Two Bone Rig", bones)
bpy.context.collection.objects.link(rig)
bpy.context.view_layer.objects.active = rig
rig.select_set(True)
bpy.ops.object.mode_set(mode="EDIT")
root = bones.edit_bones.new("Root")
root.head, root.tail = (0, 0, 0), (0, 0, 1)
arm = bones.edit_bones.new("Arm")
arm.head, arm.tail = (0, 0, 1), (0, 0, 2)
arm.parent = root
bpy.ops.object.mode_set(mode="OBJECT")

mesh = bpy.data.meshes.new("Clip Surface")
mesh.from_pydata([(x, 0, z) for z in (0, 1, 2) for x in (-0.5, 0.5)],
                 [], [(0, 1, 3, 2), (2, 3, 5, 4)])
mesh.update()
material = bpy.data.materials.new("Clip Magenta")
shader = next(node for node in material.node_tree.nodes if node.type == "BSDF_PRINCIPLED")
shader.inputs["Base Color"].default_value = (0.6, 0.03, 0.56, 1)
shader.inputs["Emission Color"].default_value = (0.5, 0, 0.48, 1)
shader.inputs["Emission Strength"].default_value = 1
mesh.materials.append(material)
uv = mesh.uv_layers.new(name="Skin UV")
for loop in mesh.loops:
    point = mesh.vertices[loop.vertex_index].co
    uv.uv[loop.index].vector = (point.x + 0.5, point.z / 2)
skin = bpy.data.objects.new("Clip Surface", mesh)
bpy.context.collection.objects.link(skin)
skin.parent = rig
rig.select_set(False)
skin.select_set(True)
bpy.context.view_layer.objects.active = skin
modifier = skin.modifiers.new("Character Skin", "ARMATURE")
modifier.object = rig
skin.vertex_groups.new(name="Root").add([0, 1, 2, 3], 1, "REPLACE")
arm_group = skin.vertex_groups.new(name="Arm")

arm = rig.pose.bones["Arm"]
arm.rotation_mode = "XYZ"
for frame, angle in ((1, 0), (7, 1), (13, 0)):
    arm.rotation_euler.z = angle
    arm.keyframe_insert("rotation_euler", frame=frame)
rig.animation_data.action.name = "Looping Bend"
bpy.context.scene.render.fps = 24
bpy.context.scene.frame_set(3)
try:
    bpy.ops.wm.publish_to_worldforge(character_clip=True)
    raise AssertionError("Studio accepted an unweighted character")
except RuntimeError as error:
    assert "positive weight" in str(error)
assert not (exchange / "ready").is_file()

arm_group.add([4, 5], 1, "REPLACE")
assert bpy.ops.wm.publish_to_worldforge(character_clip=True) == {"FINISHED"}
assert bpy.context.scene.frame_current == 3
assert bpy.context.object == skin
revision = exchange / (exchange / "ready").read_text(encoding="ascii")
worldforge_documents.open_asset(revision / "source.wfasset", remember=False, working_copy=True)
restored = bpy.data.objects["Clip Surface"]
assert restored.find_armature().animation_data.action.name == "Looping Bend"
assert len(restored.find_armature().data.bones) == 2
assert restored.vertex_groups["Arm"].weight(5) == 1
