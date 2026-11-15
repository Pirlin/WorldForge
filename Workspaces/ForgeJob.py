# SPDX-License-Identifier: GPL-2.0-or-later

"""Run a Forge AI barrier job on the Studio working copy and publish it for review.

StudioPublish.py runs first and opens the asset from the exchange folder. This script reads
forge-job.json, applies the bevel and stripe operations, publishes through the same
Publish to WorldForge operator that creators use, and writes forge-result.json.
"""

import json
import math
import os
import sys
import traceback
from pathlib import Path

import bpy
import numpy as np


exchange = Path(sys.argv[sys.argv.index("--") + 1]).resolve()
job = json.loads((exchange / "forge-job.json").read_text(encoding="utf-8"))
steps = []


class JobError(Exception):
    pass


def write_result(status, message=""):
    temporary = exchange / "forge-result.tmp"
    temporary.write_text(json.dumps({"status": status, "message": message, "steps": steps}), encoding="utf-8")
    os.replace(temporary, exchange / "forge-result.json")


def target_object():
    obj = bpy.context.view_layer.objects.active
    if obj is None or obj.type != "MESH":
        obj = next((item for item in bpy.context.scene.objects if item.type == "MESH"), None)
    if obj is None:
        raise JobError("The Studio working copy has no mesh to edit.")
    for item in bpy.context.selected_objects:
        item.select_set(False)
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)
    if obj.mode != "OBJECT":
        bpy.ops.object.mode_set(mode="OBJECT")
    return obj


def bevel(obj, settings):
    width = float(settings["width"])
    if width <= 0:
        steps.append("Kept the edges sharp.")
        return
    if any(modifier.type == "MULTIRES" for modifier in obj.modifiers):
        raise JobError("The barrier has sculpt detail, so its base edges cannot be beveled without losing the "
                       "sculpt. Set the bevel width to 0 or bevel in Modeling before sculpting.")
    faces = len(obj.data.polygons)
    bpy.ops.object.mode_set(mode="EDIT")
    try:
        bpy.ops.mesh.select_mode(type="EDGE")
        bpy.ops.mesh.select_all(action="DESELECT")
        # Edges of an earlier bevel meet at 45 degrees or less, so a repeated job leaves them alone.
        bpy.ops.mesh.edges_select_sharp(sharpness=math.radians(50))
        if not obj.data.total_edge_sel:
            bpy.ops.object.mode_set(mode="OBJECT")
            steps.append("No sharp edges were left to bevel.")
            return
        result = bpy.ops.mesh.bevel(affect="EDGES", offset=width, segments=int(settings["segments"]),
                                    clamp_overlap=True)
        bpy.ops.mesh.select_all(action="SELECT")
        bpy.ops.mesh.dissolve_degenerate(threshold=1e-5)
    finally:
        bpy.ops.object.mode_set(mode="OBJECT")
    if result != {"FINISHED"} or len(obj.data.polygons) <= faces:
        raise JobError("Studio could not bevel the barrier edges. Try a smaller bevel width.")
    steps.append(f"Beveled the sharp edges by {width:.3f} m: {faces} faces became {len(obj.data.polygons)}.")


def ensure_uvs(obj):
    if bpy.ops.mesh.worldforge_setup_uvs.poll():
        if bpy.ops.mesh.worldforge_setup_uvs() != {"FINISHED"}:
            raise JobError("Studio could not unwrap the barrier for painting.")
        steps.append("Unwrapped the barrier because it had no usable UV map.")


def smooth_noise(size, cells, rng):
    grid = rng.random((cells + 1, cells + 1))
    coordinates = np.linspace(0, cells, size, endpoint=False)
    cell = coordinates.astype(int)
    t = coordinates - cell
    t = t * t * (3 - 2 * t)
    rows = grid[cell][:, cell] * (1 - t)[None, :] + grid[cell][:, cell + 1] * t[None, :]
    below = grid[cell + 1][:, cell] * (1 - t)[None, :] + grid[cell + 1][:, cell + 1] * t[None, :]
    return rows * (1 - t)[:, None] + below * t[:, None]


def base_layer(material, size):
    tree = material.node_tree
    shader = next((node for node in tree.nodes if node.type == "BSDF_PRINCIPLED"), None)
    if shader is None:
        raise JobError(f"Material {material.name} has no Principled BSDF to paint.")
    socket = shader.inputs["Base Color"]
    if socket.is_linked and socket.links[0].from_node.type == "TEX_IMAGE" and socket.links[0].from_node.image:
        source = socket.links[0].from_node.image
        copy = source.copy()
        try:
            copy.scale(size, size)
            pixels = np.empty(size * size * 4, dtype=np.float32)
            copy.pixels.foreach_get(pixels)
        finally:
            bpy.data.images.remove(copy)
        steps.append(f"Kept the existing paint from {source.name} under the stripes.")
        return shader, pixels.reshape(size, size, 4)
    color = np.array(socket.default_value, dtype=np.float32)
    return shader, np.broadcast_to(color, (size, size, 4)).copy()


def paint_stripes(obj, settings):
    ensure_uvs(obj)
    material = obj.active_material
    if material is None:
        material = bpy.data.materials.new("Barrier Paint")
        obj.data.materials.append(material)
    size = 512
    shader, pixels = base_layer(material, size)
    rng = np.random.default_rng(int(settings["seed"]))
    u = (np.arange(size) + 0.5) / size
    uu, vv = np.meshgrid(u, u)
    count = int(settings["count"])
    band = (np.floor((uu + vv) * count) % 2) == 0
    wear = float(settings["wear"])
    fade = float(settings["fade"])
    chips = smooth_noise(size, 24, rng) * 0.65 + smooth_noise(size, 96, rng) * 0.35
    painted = band & (chips > wear * 0.55)
    stripe = np.array(settings["color"], dtype=np.float32)
    stripe = stripe * (1 - fade * 0.55) + np.array([0.82, 0.8, 0.72], dtype=np.float32) * fade * 0.55
    pixels[..., :3] = np.where(painted[..., None], stripe, pixels[..., :3])
    grime = 1 - wear * 0.3 * smooth_noise(size, 48, rng)
    pixels[..., :3] *= grime[..., None]
    pixels[..., 3] = 1
    image = bpy.data.images.new("Forge Barrier Stripes", width=size, height=size, alpha=False)
    image.pixels.foreach_set(pixels.astype(np.float32).ravel())
    image.pack()
    tree = material.node_tree
    node = tree.nodes.new("ShaderNodeTexImage")
    node.image = image
    node.location = (shader.location.x - 320, shader.location.y)
    tree.links.new(node.outputs["Color"], shader.inputs["Base Color"])
    tree.nodes.active = node
    coverage = float(painted.mean())
    steps.append(f"Painted {count} stripes on {material.name} / {image.name}; paint covers "
                 f"{coverage * 100:.0f}% of the texture after {wear * 100:.0f}% wear.")


def run():
    try:
        obj = target_object()
        bevel(obj, job["bevel"])
        paint_stripes(obj, job["stripes"])
        if bpy.ops.wm.publish_to_worldforge() != {"FINISHED"} or not (exchange / "ready").is_file():
            raise JobError("Studio could not publish the staged barrier.")
        steps.append("Published the working copy for review.")
        write_result("PASS")
    except JobError as error:
        write_result("FAIL", str(error))
    except Exception:
        write_result("FAIL", traceback.format_exc(limit=4))


run()
