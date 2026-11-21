# SPDX-License-Identifier: GPL-2.0-or-later

"""Connect the embedded Studio session to its WorldForge asset."""

import json
from array import array
import math
import os
import shutil
from pathlib import Path
import sys
import tempfile
import struct

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
    uv = mesh.uv_layers.new(name=topology.get("activeUv") or "UV Map")
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
    for layout in topology.get("uvLayers", []):
        layer = mesh.uv_layers.new(name=layout["name"], do_init=False)
        for entry, coordinate in zip(layer.uv, layout["coordinates"], strict=True):
            entry.vector = coordinate
    mesh.uv_layers.active_index = 0
    mesh.uv_layers[0].active_render = True
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
    if "--authoring-tools-only" not in options:
        publish_row = layout.row()
        publish_row.operator_context = "INVOKE_DEFAULT"
        publish = publish_row.operator("wm.publish_to_worldforge", text="Publish Game Mesh", icon="EXPORT")
        publish.triangle_budget = 20000


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

    triangle_budget: bpy.props.IntProperty(
        name="Triangle Budget", default=0, min=0, max=1500000, soft_min=100, soft_max=200000,
        description="Maximum triangles for the game mesh. Zero keeps the current evaluated resolution",
    )
    bake_detail: bpy.props.BoolProperty(
        name="Bake Sculpt Detail",
        default=False,
        description="Bake a tangent-space normal map from the sculpt onto the reduced mesh",
    )
    character_clip: bpy.props.BoolProperty(default=False, options={"HIDDEN", "SKIP_SAVE"})

    def invoke(self, context, _event):
        if self.triangle_budget:
            return context.window_manager.invoke_props_dialog(self, title="Publish Game Mesh")
        return self.execute(context)

    def draw(self, _context):
        self.layout.prop(self, "triangle_budget")
        self.layout.prop(self, "bake_detail")
        self.layout.label(text="The editable sculpt is saved at its original resolution.")

    @staticmethod
    def triangle_count(context, obj):
        context.view_layer.update()
        evaluated = obj.evaluated_get(context.evaluated_depsgraph_get()).data
        evaluated.calc_loop_triangles()
        return len(evaluated.loop_triangles)

    @staticmethod
    def sample_character(context, obj, path):
        armatures = [item.object for item in obj.modifiers if item.type == "ARMATURE" and item.show_viewport]
        if len(armatures) != 1 or armatures[0] is None or len(obj.modifiers) != 1 or obj.data.shape_keys:
            raise RuntimeError("Select a weighted mesh with one active Armature modifier and no other modifiers or shape keys.")
        armature = armatures[0]
        animation = armature.animation_data
        action = animation.action if animation else None
        if action is None or any(track.strips and not track.mute for track in animation.nla_tracks):
            raise RuntimeError("Assign one active Action to the armature and mute any NLA tracks.")
        bones = {bone.name for bone in armature.data.bones if bone.use_deform}
        groups = {group.index for group in obj.vertex_groups if group.name in bones}
        if not bones or not obj.data.vertices or any(
            not any(weight.group in groups and weight.weight > 0 for weight in vertex.groups)
            for vertex in obj.data.vertices
        ):
            raise RuntimeError("Every character vertex needs a positive weight on a deform bone.")
        first, last = math.floor(action.frame_range[0]), math.ceil(action.frame_range[1])
        count = last - first + 1
        vertices = len(obj.data.vertices)
        fps = context.scene.render.fps / context.scene.render.fps_base
        name = action.name.encode("utf-8")
        if not 2 <= count <= 120 or count * vertices > 500_000 or not 1 <= fps <= 120 or len(name) > 128:
            raise RuntimeError("Use a 2–120 frame action and at most 500,000 vertex samples.")
        scene = context.scene
        timeline = (scene.frame_current, scene.frame_subframe)
        poses = array("f")
        try:
            for frame in range(first, last + 1):
                scene.frame_set(frame)
                evaluated = obj.evaluated_get(context.evaluated_depsgraph_get())
                mesh = evaluated.data
                if len(mesh.vertices) != vertices:
                    raise RuntimeError("The character changes vertex count during the Action.")
                transform = evaluated.matrix_world
                normal_matrix = transform.to_3x3().inverted().transposed()
                for vertex in mesh.vertices:
                    poses.extend(transform @ vertex.co)
                    poses.extend((normal_matrix @ vertex.normal).normalized())
            if sys.byteorder != "little":
                poses.byteswap()
            with path.open("wb") as output:
                output.write(struct.pack("<4sIIfI", b"WFM1", count, vertices, fps, len(name)))
                output.write(name)
                poses.tofile(output)
            scene.frame_set(first)
        except (OSError, RuntimeError, ValueError):
            scene.frame_set(timeline[0], subframe=timeline[1])
            raise
        return timeline

    @staticmethod
    def bake_normal_map(context, high, low, image, materials):
        if not bpy.app.build_options.cycles:
            raise RuntimeError("Cycles is required to bake sculpt detail.")
        if low.data.uv_layers.active is None:
            raise RuntimeError("Unwrap the sculpt's UVs before baking detail.")
        targets = []
        for index, existing in enumerate(low.data.materials[:] or [None]):
            material = existing.copy() if existing else bpy.data.materials.new("Sculpt Surface")
            materials.append(material)
            if material.node_tree is None:
                material.use_nodes = True
            nodes = material.node_tree.nodes
            output = next((node for node in nodes if node.type == "OUTPUT_MATERIAL" and node.is_active_output), None)
            links = output.inputs["Surface"].links if output else ()
            shader = links[0].from_node if links else None
            if shader is None or shader.type != "BSDF_PRINCIPLED":
                raise RuntimeError("Baking detail requires one Principled shader connected to each material output.")
            target = nodes.new("ShaderNodeTexImage")
            target.name = "Sculpt Detail"
            target.image = image
            nodes.active = target
            targets.append((material.node_tree, target, shader))
            if index == len(low.data.materials):
                low.data.materials.append(material)
            else:
                low.data.materials[index] = material

        render = context.scene.render
        cycles = context.scene.cycles
        engine, device = render.engine, cycles.device
        try:
            render.engine = "CYCLES"
            cycles.device = "CPU"
            high.select_set(True)
            low.select_set(True)
            context.view_layer.objects.active = low
            cage = max(0.001, max(high.dimensions) * 0.05)
            if bpy.ops.object.bake(type="NORMAL", normal_space="TANGENT", use_selected_to_active=True,
                                   cage_extrusion=cage, max_ray_distance=cage * 2, margin=12) != {"FINISHED"}:
                raise RuntimeError("Studio could not bake the sculpt detail.")
            image.pack()
        finally:
            render.engine = engine
            cycles.device = device
        for tree, target, shader in targets:
            normal = tree.nodes.new("ShaderNodeNormalMap")
            tree.links.new(target.outputs["Color"], normal.inputs["Color"])
            tree.links.new(normal.outputs["Normal"], shader.inputs["Normal"])

    def execute(self, context):
        global published_manifest
        obj = context.active_object
        if obj is None or obj.type != "MESH":
            self.report({"ERROR"}, "Select the mesh you want to publish.")
            return {"CANCELLED"}
        if len(obj.data.uv_layers) > 8 or (not self.character_clip and obj.parent is not None):
            self.report({"ERROR"}, "Publish an unparented mesh with up to eight UV layers.")
            return {"CANCELLED"}
        if not self.character_clip and (obj.animation_data or obj.data.shape_keys
                                        or any(modifier.type == "ARMATURE" for modifier in obj.modifiers)):
            self.report({"ERROR"}, "Use Publish Character Clip for a weighted mesh with an active armature Action.")
            return {"CANCELLED"}
        selected = list(context.selected_objects)
        previous_mode = obj.mode
        revision = None
        reduction = None
        reduction_report = ""
        low = None
        low_mesh = None
        bake_image = None
        bake_materials = []
        timeline = None

        def discard_bake():
            nonlocal low, low_mesh, bake_image
            if low is not None:
                bpy.data.objects.remove(low, do_unlink=True)
                low = None
            if low_mesh is not None:
                bpy.data.meshes.remove(low_mesh)
                low_mesh = None
            for material in bake_materials:
                bpy.data.materials.remove(material)
            bake_materials.clear()
            if bake_image is not None:
                bpy.data.images.remove(bake_image)
                bake_image = None

        try:
            if obj.mode != "OBJECT":
                bpy.ops.object.mode_set(mode="OBJECT")
            for item in selected:
                item.select_set(False)
            obj.select_set(True)
            revision = Path(tempfile.mkdtemp(prefix="publish-", dir=exchange))
            staged = revision / "mesh.glb"
            if self.character_clip:
                timeline = self.sample_character(context, obj, revision / "clip.wfmotion")
                depsgraph = context.evaluated_depsgraph_get()
                evaluated = obj.evaluated_get(depsgraph)
                low_mesh = bpy.data.meshes.new_from_object(evaluated, preserve_all_data_layers=True, depsgraph=depsgraph)
                vertex_map = low_mesh.attributes.new("_WORLDFORGE_VERTEX", "FLOAT", "POINT")
                vertex_map.data.foreach_set("value", array("f", range(len(low_mesh.vertices))))
                low = bpy.data.objects.new(obj.name + " Runtime Mesh", low_mesh)
                context.collection.objects.link(low)
                low.matrix_world = evaluated.matrix_world.copy()
                obj.select_set(False)
                low.select_set(True)
                context.view_layer.objects.active = low
            if self.triangle_budget and not self.character_clip:
                before = self.triangle_count(context, obj)
                after = before
                if before > self.triangle_budget:
                    reduction = obj.modifiers.new("WorldForge Publish Reduction", "DECIMATE")
                    reduction.use_collapse_triangulate = True
                    reduction.ratio = self.triangle_budget / before
                    after = self.triangle_count(context, obj)
                if after == 0 or after > self.triangle_budget:
                    raise RuntimeError("The mesh could not meet the triangle budget. Choose a larger budget.")
                reduction_report = f" {before:,} → {after:,} triangles."
            if self.bake_detail and not self.character_clip:
                if reduction is None:
                    raise RuntimeError("Choose a triangle budget below the sculpt resolution to bake detail.")
                depsgraph = context.evaluated_depsgraph_get()
                low_mesh = bpy.data.meshes.new_from_object(obj.evaluated_get(depsgraph), preserve_all_data_layers=True,
                                                          depsgraph=depsgraph)
                low = bpy.data.objects.new(obj.name + " Game Mesh", low_mesh)
                context.collection.objects.link(low)
                low.matrix_world = obj.matrix_world.copy()
                obj.modifiers.remove(reduction)
                reduction = None
                bake_image = bpy.data.images.new("Sculpt Detail Normal", width=512, height=512)
                bake_image.colorspace_settings.name = "Non-Color"
                self.bake_normal_map(context, obj, low, bake_image, bake_materials)
                obj.select_set(False)
                reduction_report += " Sculpt detail baked."
            context.view_layer.update()
            exported = (low if low is not None else obj).evaluated_get(context.evaluated_depsgraph_get()).data
            uv_manifest = {"version": 1, "names": [layer.name for layer in exported.uv_layers],
                           "render": max(0, exported.uv_layers.active_render_index)}
            result = bpy.ops.export_scene.gltf(filepath=str(staged), export_format="GLB",
                                               use_selection=True, export_apply=True,
                                               export_animations=False, export_skins=False,
                                               export_morph=False, export_attributes=self.character_clip)
            if result != {"FINISHED"} or not staged.is_file():
                raise RuntimeError("Studio could not export the selected mesh.")
            if staged.stat().st_size > 64 * 1024 * 1024:
                raise RuntimeError("The evaluated mesh exceeds the 64 MiB publishing limit.")
            (revision / "uv-sets.json").write_text(json.dumps(uv_manifest), encoding="utf-8")
            if reduction is not None:
                obj.modifiers.remove(reduction)
                reduction = None
            discard_bake()
            if timeline is not None:
                context.scene.frame_set(timeline[0], subframe=timeline[1])
                timeline = None
            obj.select_set(True)
            context.view_layer.objects.active = obj
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
            if timeline is not None:
                context.scene.frame_set(timeline[0], subframe=timeline[1])
            if reduction is not None:
                obj.modifiers.remove(reduction)
            discard_bake()
            context.view_layer.objects.active = obj
            obj.select_set(False)
            for item in selected:
                item.select_set(True)
            if obj.mode != previous_mode:
                try:
                    bpy.ops.object.mode_set(mode=previous_mode)
                except RuntimeError as error:
                    self.report({"WARNING"}, f"Could not restore {previous_mode} mode. {error}")
        self.report({"INFO"}, "Mesh ready. Select Apply Studio Update in WorldForge." + reduction_report)
        return {"FINISHED"}


def draw_publish(self, context):
    self.layout.operator(WM_OT_publish_to_worldforge.bl_idname)
    obj = context.active_object
    if obj is not None and obj.type == "MESH" and obj.find_armature() is not None:
        self.layout.operator(WM_OT_publish_to_worldforge.bl_idname, text="Publish Character Clip").character_clip = True


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
