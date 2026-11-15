# SPDX-License-Identifier: GPL-2.0-or-later

"""Connect the embedded Studio session to its WorldForge asset."""

import json
import os
import shutil
from pathlib import Path
import sys
import tempfile

import bmesh
import bpy


exchange_arg, *options = sys.argv[sys.argv.index("--") + 1:]
exchange = Path(exchange_arg).resolve()
published_manifest = None


def restore_authoring_topology(obj):
    imported = obj.data
    encoded = imported.get("worldforgeTopology")
    if encoded is None:
        return
    topology = json.loads(encoded)
    slots = {material["worldforgeSlot"]: material for material in imported.materials}
    slot_indices = {slot: index for index, slot in enumerate(slots)}
    faces = topology["faces"]
    mesh = bpy.data.meshes.new(imported.name)
    mesh.from_pydata(topology["vertices"], [], [[corner[0] for corner in corners] for _, corners in faces])
    uv = mesh.uv_layers.new(name="UV Map")
    normals = [tuple(corner[3:]) for _, corners in faces for corner in corners]
    for material in slots.values():
        del material["worldforgeSlot"]
        mesh.materials.append(material)
    for polygon, (slot, corners) in zip(mesh.polygons, faces, strict=True):
        polygon.material_index = slot_indices[slot]
        polygon.use_smooth = any(any(corner[3:]) for corner in corners)
        for loop, corner in zip(polygon.loop_indices, corners, strict=True):
            uv.uv[loop].vector = corner[1:3]
    if any(any(normal) for normal in normals):
        mesh.normals_split_custom_set(normals)
    mesh.update()
    obj.data = mesh
    if imported.users == 0:
        bpy.data.meshes.remove(imported)


def is_split_box(obj):
    if obj is None or obj.type != "MESH" or obj.mode not in {"OBJECT", "EDIT"} or obj.modifiers:
        return False
    mesh = obj.data
    if (not obj.is_editable or mesh.users != 1 or not mesh.is_editable or mesh.shape_keys
            or obj.vertex_groups or len(mesh.polygons) != 12 or len(mesh.vertices) not in {8, 24}):
        return False
    allowed = {"position", "custom_normal", "sharp_face", "sharp_edge", *(uv.name for uv in mesh.uv_layers)}
    if any(not attribute.is_internal and attribute.name not in allowed for attribute in mesh.attributes):
        return False
    points = {tuple(vertex.co) for vertex in mesh.vertices}
    return (len(points) == 8 and all(len({point[axis] for point in points}) == 2 for axis in range(3))
            and all(face.loop_total == 3 for face in mesh.polygons))


class MESH_OT_worldforge_restore_box_faces(bpy.types.Operator):
    bl_idname = "mesh.worldforge_restore_box_faces"
    bl_label = "Restore Box Faces"
    bl_description = "Remove the six diagonals left by the earlier Studio handoff and restore connected quad faces"
    bl_options = {"UNDO"}

    @classmethod
    def poll(cls, context):
        return is_split_box(context.active_object)

    def execute(self, context):
        obj = context.active_object
        mode = obj.mode
        if mode == "EDIT":
            bpy.ops.object.mode_set(mode="OBJECT")
        bm = bmesh.new()
        try:
            if not is_split_box(obj):
                return {"CANCELLED"}
            bm.from_mesh(obj.data)
            bmesh.ops.join_triangles(bm, faces=list(bm.faces), cmp_uvs=True, cmp_materials=True,
                                     cmp_seam=True, cmp_sharp=True,
                                     angle_face_threshold=0.001, angle_shape_threshold=0.001)
            if len(bm.faces) != 6 or any(len(face.verts) != 4 for face in bm.faces):
                self.report({"WARNING"}, "The triangles could not be safely joined across matching UVs and materials.")
                return {"CANCELLED"}
            positions = {}
            duplicates = {}
            for vertex in bm.verts:
                original = positions.setdefault(tuple(vertex.co), vertex)
                if original != vertex:
                    duplicates[vertex] = original
            if duplicates:
                bmesh.ops.weld_verts(bm, targetmap=duplicates)
            if len(bm.verts) != 8 or len(bm.edges) != 12 or any(not edge.is_manifold for edge in bm.edges):
                self.report({"WARNING"}, "The resulting faces did not form a closed box.")
                return {"CANCELLED"}
            bm.to_mesh(obj.data)
            obj.data.update()
            return {"FINISHED"}
        finally:
            bm.free()
            if mode == "EDIT":
                bpy.ops.object.mode_set(mode="EDIT")


def has_paint_uvs(mesh):
    layer = mesh.uv_layers.active
    if layer is None:
        return False
    uvs = layer.uv
    for polygon in mesh.polygons:
        previous = uvs[polygon.loop_indices[-1]].vector
        area = 0.0
        for loop_index in polygon.loop_indices:
            current = uvs[loop_index].vector
            area += previous.x * current.y - current.x * previous.y
            previous = current
        if abs(area) > 1e-12:
            return True
    return False


class MESH_OT_worldforge_setup_uvs(bpy.types.Operator):
    bl_idname = "mesh.worldforge_setup_uvs"
    bl_label = "Set Up UVs"
    bl_description = "Unwrap the selected mesh for painting when its active UV map is missing or collapsed"
    bl_options = {"UNDO"}

    @classmethod
    def poll(cls, context):
        obj = context.active_object
        return (obj is not None and obj.type == "MESH" and obj.mode in {"OBJECT", "TEXTURE_PAINT"}
                and obj.is_editable and obj.data.is_editable and obj.data.users == 1
                and bool(obj.data.polygons) and not has_paint_uvs(obj.data))

    def execute(self, context):
        obj = context.active_object
        mesh = obj.data
        mode = obj.mode
        layer = mesh.uv_layers.active
        old_uvs = [tuple(uv.vector) for uv in layer.uv] if layer else []
        others = [selected for selected in context.selected_objects if selected != obj]
        selection = [
            [(element.select, element.hide) for element in domain]
            for domain in (mesh.vertices, mesh.edges, mesh.polygons)
        ]
        finished = False
        try:
            bpy.ops.object.mode_set(mode="OBJECT")
            for other in others:
                other.select_set(False)
            if bpy.ops.object.mode_set(mode="EDIT") == {"FINISHED"}:
                bpy.ops.mesh.reveal(select=True)
                bpy.ops.mesh.select_all(action="SELECT")
                result = bpy.ops.uv.smart_project(island_margin=0.025)
                bpy.ops.object.mode_set(mode="OBJECT")
                finished = result == {"FINISHED"} and has_paint_uvs(mesh)
        except RuntimeError as error:
            self.report({"WARNING"}, str(error))
        finally:
            if obj.mode == "EDIT":
                bpy.ops.object.mode_set(mode="OBJECT")
            for domain, flags in zip((mesh.vertices, mesh.edges, mesh.polygons), selection, strict=True):
                for element, (selected, hidden) in zip(domain, flags, strict=True):
                    element.select, element.hide = selected, hidden
            if not finished:
                if layer is None and mesh.uv_layers.active is not None:
                    mesh.uv_layers.remove(mesh.uv_layers.active)
                elif layer is not None:
                    for uv, coordinate in zip(layer.uv, old_uvs, strict=True):
                        uv.vector = coordinate
            mesh.update()
            for other in others:
                other.select_set(True)
            if mode != "OBJECT":
                bpy.ops.object.mode_set(mode=mode)
        if not finished:
            self.report({"WARNING"}, "Could not unwrap this mesh. Check that it has faces with surface area.")
            return {"CANCELLED"}
        return {"FINISHED"}


def paint_target(context):
    """Return the material and image that the next Texture Paint stroke will change.

    Studio refreshes the material's cached paint slots when a stroke starts, and the refresh
    selects the active image node. The cache can be stale after an image is assigned in the
    shader editor, so read the active image node first.
    """
    paint = context.tool_settings.image_paint
    if paint.mode == "IMAGE":
        return None, paint.canvas
    material = context.active_object.active_material
    if material is None:
        return None, None
    if material.node_tree:
        node = material.node_tree.nodes.active
        if node is not None and node.type == "TEX_IMAGE" and node.image is not None:
            return material, node.image
    index = material.paint_active_slot
    images = material.texture_paint_images
    return material, images[index] if 0 <= index < len(images) else None


SCULPT_BRUSHES = ("Draw", "Smooth")
SCULPT_DETAIL_LIMIT = 128


def sculpt_detail_status(obj):
    """Explain whether Add Sculpt Detail can run on the sculpt object, or return None."""
    if obj is None or obj.type != "MESH":
        return "Select a mesh to sculpt"
    detail = next((modifier for modifier in obj.modifiers if modifier.type == "MULTIRES"), None)
    if detail is not None:
        return f"Sculpt detail level {detail.sculpt_levels} of {detail.total_levels}"
    if obj.use_dynamic_topology_sculpting:
        return "Dynamic topology adds detail while you sculpt"
    if not obj.is_editable or not obj.data.is_editable or obj.data.users != 1:
        return "Sculpt detail needs an editable mesh used by one object"
    if obj.data.shape_keys is not None:
        return "Remove shape keys before adding sculpt detail"
    if obj.modifiers:
        return "Apply or remove modifiers before adding sculpt detail"
    if not obj.data.polygons:
        return "Sculpt detail needs a mesh with faces"
    if len(obj.data.polygons) > SCULPT_DETAIL_LIMIT or len(obj.data.vertices) > SCULPT_DETAIL_LIMIT:
        return "This mesh has enough detail to sculpt"
    return None


def activate_sculpt_brush(name):
    return bpy.ops.brush.asset_activate(
        asset_library_type="ESSENTIALS",
        relative_asset_identifier=f"brushes/essentials_brushes-mesh_sculpt.blend/Brush/{name}")


class SCULPT_OT_worldforge_brush(bpy.types.Operator):
    bl_idname = "sculpt.worldforge_brush"
    bl_label = "Sculpt Brush"
    bl_description = "Switch to a basic sculpting brush"
    bl_options = {"INTERNAL"}

    brush: bpy.props.EnumProperty(items=[(name, name, "") for name in SCULPT_BRUSHES])

    @classmethod
    def description(cls, _context, properties):
        if properties.brush == "Smooth":
            return "Even out bumps and sharp creases in the sculpted surface"
        return "Raise or carve the surface under the brush. Hold Ctrl to carve"

    @classmethod
    def poll(cls, context):
        return context.mode == "SCULPT"

    def execute(self, _context):
        try:
            return activate_sculpt_brush(self.brush)
        except RuntimeError as error:
            self.report({"WARNING"}, str(error))
            return {"CANCELLED"}


def draw_sculpt_actions(layout, context):
    brush = context.tool_settings.sculpt.brush
    row = layout.row(align=True)
    for name in SCULPT_BRUSHES:
        icon = "SCULPTMODE_HLT" if name == "Draw" else "MOD_SMOOTH"
        button = row.operator(SCULPT_OT_worldforge_brush.bl_idname, text=name, icon=icon,
                              depress=brush is not None and brush.name == name)
        button.brush = name
    status = sculpt_detail_status(context.sculpt_object)
    if status is not None:
        layout.label(text=status, icon="INFO")


def draw_authoring_actions(self, context):
    obj = context.active_object
    if context.mode == "SCULPT" and obj and obj.type == "MESH":
        draw_sculpt_actions(self.layout, context)
    elif context.workspace.name == "Modeling" and obj and obj.type == "MESH":
        self.layout.prop(context.tool_settings, "transform_pivot_point", text="Set Pivot")
        if context.mode == "EDIT_MESH":
            if any(item.data.total_edge_sel for item in context.objects_in_mode_unique_data):
                row = self.layout.row()
                row.operator_context = "INVOKE_REGION_WIN"
                bevel = row.operator("mesh.bevel", text="Bevel Edges", icon="MOD_BEVEL")
                bevel.affect = "EDGES"
            else:
                self.layout.label(text="Select edges to bevel")
        if is_split_box(obj):
            self.layout.operator(MESH_OT_worldforge_restore_box_faces.bl_idname, icon="FACESEL")
    elif context.mode == "PAINT_TEXTURE" and obj and obj.type == "MESH":
        if not has_paint_uvs(obj.data):
            if MESH_OT_worldforge_setup_uvs.poll(context):
                self.layout.operator(MESH_OT_worldforge_setup_uvs.bl_idname, icon="UV")
            else:
                self.layout.label(text="UV setup needs an editable, single-user mesh with faces", icon="INFO")
            return
        paint = context.tool_settings.image_paint
        material, image = paint_target(context)
        if image is not None:
            target = f"{material.name} / {image.name}" if material else image.name
            self.layout.label(text=target, icon="IMAGE_DATA")
        elif paint.mode == "MATERIAL" and (material is None or not material.texture_paint_images):
            button = self.layout.row()
            button.operator_context = "INVOKE_DEFAULT"
            action = button.operator("paint.add_texture_paint_slot", text="Create Paint Layer", icon="ADD")
            action.type = "BASE_COLOR"
            action.slot_type = "IMAGE"
        else:
            self.layout.label(text="Select a paint image", icon="INFO")


class WM_OT_publish_to_worldforge(bpy.types.Operator):
    bl_idname = "wm.publish_to_worldforge"
    bl_label = "Publish to WorldForge"
    bl_description = "Prepare the active mesh for review in WorldForge Editor"

    def execute(self, context):
        global published_manifest
        obj = context.active_object
        if obj is None or obj.type != "MESH":
            self.report({"ERROR"}, "Select the mesh you want to publish.")
            return {"CANCELLED"}
        if len(obj.data.uv_layers) > 1 or obj.parent is not None:
            self.report({"ERROR"}, "Publish a mesh with one UV set and no parent in this exchange profile.")
            return {"CANCELLED"}
        if obj.animation_data or obj.data.shape_keys or any(modifier.type == "ARMATURE" for modifier in obj.modifiers):
            self.report({"ERROR"}, "Publish a static mesh without animation, shape keys, or an armature.")
            return {"CANCELLED"}
        selected = list(context.selected_objects)
        previous_mode = obj.mode
        revision = None
        try:
            if obj.mode != "OBJECT":
                bpy.ops.object.mode_set(mode="OBJECT")
            for item in selected:
                item.select_set(False)
            obj.select_set(True)
            revision = Path(tempfile.mkdtemp(prefix="publish-", dir=exchange))
            staged = revision / "mesh.glb"
            result = bpy.ops.export_scene.gltf(filepath=str(staged), export_format="GLB",
                                               use_selection=True, export_apply=True,
                                               export_animations=False, export_skins=False,
                                               export_morph=False)
            if result != {"FINISHED"} or not staged.is_file():
                raise RuntimeError("Studio could not export the selected mesh.")
            if staged.stat().st_size > 64 * 1024 * 1024:
                raise RuntimeError("The evaluated mesh exceeds the 64 MiB publishing limit.")
            from bl_operators import worldforge_documents
            source = revision / "source.wfasset"
            manifest = worldforge_documents.snapshot_asset(source, published_manifest)
            if source.stat().st_size > 128 * 1024 * 1024:
                raise RuntimeError("The editable package exceeds the 128 MiB publishing limit.")
            ready = revision / "ready"
            ready.write_text(revision.name, encoding="ascii")
            os.replace(ready, exchange / "ready")
            published_manifest = manifest
        except (OSError, RuntimeError, ValueError) as error:
            if revision is not None:
                shutil.rmtree(revision, ignore_errors=True)
            self.report({"ERROR"}, str(error))
            return {"CANCELLED"}
        finally:
            obj.select_set(False)
            for item in selected:
                item.select_set(True)
            if obj.mode != previous_mode:
                try:
                    bpy.ops.object.mode_set(mode=previous_mode)
                except RuntimeError as error:
                    self.report({"WARNING"}, f"Could not restore {previous_mode} mode. {error}")
        self.report({"INFO"}, "Mesh ready. Select Apply Studio Update in WorldForge.")
        return {"FINISHED"}


def draw_publish(self, _context):
    self.layout.operator(WM_OT_publish_to_worldforge.bl_idname)


if "--authoring-tools-only" not in options:
    source = exchange / "source.wfasset"
    if source.is_file():
        from bl_operators import worldforge_documents
        published_manifest = worldforge_documents.open_asset(source, remember=False, working_copy=True)
    else:
        for item in list(bpy.context.scene.objects):
            bpy.data.objects.remove(item, do_unlink=True)
        input_mesh = exchange / "input.glb"
        if input_mesh.stat().st_size:
            if bpy.ops.import_scene.gltf(filepath=str(input_mesh)) != {"FINISHED"}:
                raise RuntimeError("Studio could not open the WorldForge mesh.")
            restore_authoring_topology(bpy.context.active_object)
    bpy.utils.register_class(WM_OT_publish_to_worldforge)
    bpy.types.TOPBAR_MT_file.append(draw_publish)
bpy.utils.register_class(MESH_OT_worldforge_restore_box_faces)
bpy.utils.register_class(MESH_OT_worldforge_setup_uvs)
bpy.utils.register_class(SCULPT_OT_worldforge_brush)
if not bpy.app.background:
    bpy.types.VIEW3D_HT_tool_header.prepend(draw_authoring_actions)
