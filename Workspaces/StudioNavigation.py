# SPDX-License-Identifier: GPL-2.0-or-later

"""Select Studio workspaces from the WorldForge Editor navigation."""

from pathlib import Path
import sys

import bpy


exchange_arg, *options = sys.argv[sys.argv.index("--") + 1:]
EXCHANGE = Path(exchange_arg)
WORKSPACES = {
    "Modeling": "Modeling",
    "Sculpting": "Sculpting",
    "UV Editing": "UV Editing",
    "Texture": "Texture Paint",
    "Material": "Shading",
    "Rigging": "Rigging",
    "Animation": "Animation",
}
last_request = None


def mesh_shape(mesh):
    return (tuple(tuple(vertex.co) for vertex in mesh.vertices),
            tuple(tuple(face.vertices) for face in mesh.polygons))


def starter_cube():
    obj = bpy.data.objects.get("Cube")
    if bpy.data.filepath or obj is None or obj.type != "MESH" or len(obj.data.vertices) != 8:
        return None
    return obj.session_uid, obj.data.session_uid, mesh_shape(obj.data)


starter = None if "--asset-session" in options else starter_cube()


def prepare_starter_for_sculpting():
    global starter
    context = bpy.context
    obj = context.sculpt_object
    if starter is None or obj is None or obj.type != "MESH":
        return
    object_id, mesh_id, shape = starter
    if (obj.session_uid != object_id or obj.data.session_uid != mesh_id
            or len(obj.data.vertices) != 8 or mesh_shape(obj.data) != shape):
        return
    starter = None
    viewport = next((area for area in context.screen.areas if area.type == "VIEW_3D"), None)
    if viewport is None:
        return
    region = next((region for region in viewport.regions if region.type == "WINDOW"), None)
    if region is None:
        return
    with context.temp_override(area=viewport, region=region):
        if bpy.ops.object.worldforge_add_sculpt_detail.poll():
            bpy.ops.object.worldforge_add_sculpt_detail()


def rigging_workspace():
    existing = set(bpy.data.workspaces)
    bpy.ops.workspace.duplicate()
    workspace, = set(bpy.data.workspaces) - existing
    workspace.name = "Rigging"
    return workspace


def switch_workspace():
    global last_request
    try:
        request = (EXCHANGE / "workspace").read_bytes()
        selection = request.split(b"\n", 1)[-1].decode("utf-8")
        window = next(iter(bpy.context.window_manager.windows), None)
        if request == last_request or window is None or selection not in WORKSPACES:
            return 0.15
        with bpy.context.temp_override(window=window):
            name = WORKSPACES[selection]
            workspace = bpy.data.workspaces.get(name)
            if workspace is None and name == "Rigging":
                modeling = bpy.data.workspaces.get("Modeling")
                if modeling is not None and window.workspace != modeling:
                    window.workspace = modeling
                    return 0.15
                workspace = rigging_workspace()
            if workspace is None:
                raise RuntimeError(f"Studio workspace {name} is missing.")
            if window.workspace != workspace:
                window.workspace = workspace
                return 0.05
            if selection == "Sculpting":
                prepare_starter_for_sculpting()
        ready = EXCHANGE / "workspace-ready.tmp"
        ready.write_bytes(request)
        ready.replace(EXCHANGE / "workspace-ready")
        last_request = request
    except (OSError, RuntimeError, ValueError) as error:
        print(f"WorldForge Studio navigation: {error}", file=sys.stderr)
    return 0.15


class WM_OT_worldforge_toggle_uv(bpy.types.Operator):
    bl_idname = "wm.worldforge_toggle_uv"
    bl_label = "Switch UV Workspace"

    def execute(self, context):
        name = "Modeling" if context.workspace.name == "UV Editing" else "UV Editing"
        workspace = bpy.data.workspaces.get(name)
        if workspace is None:
            self.report({"ERROR"}, f"The {name} workspace is unavailable.")
            return {"CANCELLED"}
        context.window.workspace = workspace
        return {"FINISHED"}


def draw_uv_workspace(self, context):
    if context.workspace.name in {"Modeling", "UV Editing"}:
        label = "Return to Modeling" if context.workspace.name == "UV Editing" else "UV Editing"
        self.layout.separator()
        self.layout.operator("wm.worldforge_toggle_uv", text=label, icon="UV")


if not bpy.app.background:
    bpy.utils.register_class(WM_OT_worldforge_toggle_uv)
    bpy.types.TOPBAR_MT_window.append(draw_uv_workspace)
    bpy.app.timers.register(switch_workspace, first_interval=0.05, persistent=True)
