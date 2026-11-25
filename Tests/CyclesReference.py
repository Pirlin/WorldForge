# Renders the Cycles reference for one Player frame, for the realism check.
#
#   WorldForgeStudio.exe -b --factory-startup --python Tests/CyclesReference.py -- <folder> [samples]
#
# <folder> comes from `WorldForgePlayer --reference`: scene.json, scene.bin and texture-n.png. The script
# writes cycles.png and cycles.json (render time and settings) into the same folder.
#
# The reference is a physical reading of the same scene description:
# - Engine light intensity I is in scene units (one unit is 123,532 lux; see Engine/Render/ColorPipeline.h):
#   a white surface facing the light reflects radiance I, which is what the engine's lighting assumes.
#   Cycles is physical (a sun of strength S gives a white surface S/pi), so a sun becomes strength pi*I and
#   a point light pi*I*4*pi watts, falling off with the inverse square.
# - The visible sky is the only ambient light, as in the engine (Engine/Render/SkyLight.h). The engine
#   approximates it with spherical harmonics and a flat ground below the horizon and has no occlusion
#   yet; Cycles traces it, so geometry blocks it.
# - The camera uses the scene's manual exposure; auto exposure is not part of the comparison.
# - Materials use the engine's base color, metallic, roughness, normal and emission inputs with the
#   Principled BSDF, so every surface has the specular reflection a real dielectric has.

import json
import math
import os
import sys
import time

import _cycles
import bpy
import numpy
from mathutils import Vector

arguments = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
if not arguments:
    raise SystemExit("Usage: -- <folder> [samples]")
folder = arguments[0]
samples = int(arguments[1]) if len(arguments) > 1 else 256

with open(os.path.join(folder, "scene.json"), encoding="utf-8") as file:
    description = json.load(file)
data = numpy.fromfile(os.path.join(folder, "scene.bin"), dtype="<f4").reshape(-1, 11)
if len(data) != description["vertices"]:
    raise SystemExit("scene.bin does not match scene.json")


def srgb_to_linear(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def linear(rgb):
    return [srgb_to_linear(c) for c in rgb]


def input_of(node, identifier):
    return next(socket for socket in node.inputs if socket.identifier == identifier)


def output_of(node, identifier):
    return next(socket for socket in node.outputs if socket.identifier == identifier)


scene = bpy.context.scene
for item in list(bpy.data.objects):
    bpy.data.objects.remove(item)

# Textures, loaded once and shared by the materials that use them.
images = {}


def texture(index, color_space):
    key = (index, color_space)
    if key not in images:
        image = bpy.data.images.load(os.path.join(folder, "texture-%d.png" % index))
        image.colorspace_settings.name = color_space
        images[key] = image
    return images[key]


def make_material(name, range_):
    material = bpy.data.materials.new(name)
    if material.node_tree is None:
        material.use_nodes = True
    nodes, links = material.node_tree.nodes, material.node_tree.links
    shader = nodes["Principled BSDF"]
    textures = range_["textures"]
    vertex_color = nodes.new("ShaderNodeVertexColor")
    vertex_color.layer_name = "Color"
    base = output_of(vertex_color, "Color")
    if textures[0] >= 0:
        image = nodes.new("ShaderNodeTexImage")
        image.image = texture(textures[0], "sRGB")
        multiply = nodes.new("ShaderNodeMix")
        multiply.data_type = "RGBA"
        multiply.blend_type = "MULTIPLY"
        input_of(multiply, "Factor_Float").default_value = 1
        links.new(base, input_of(multiply, "A_Color"))
        links.new(output_of(image, "Color"), input_of(multiply, "B_Color"))
        base = output_of(multiply, "Result_Color")
    links.new(base, shader.inputs["Base Color"])
    metallic, roughness = range_["metallic"], max(0.045, range_["roughness"])
    shader.inputs["Metallic"].default_value = metallic
    shader.inputs["Roughness"].default_value = roughness
    shader.inputs["Coat Weight"].default_value = range_.get("clearCoat", 0)
    shader.inputs["Coat Roughness"].default_value = range_.get("clearCoatRoughness", 0)
    sheen = range_.get("sheenColor", (0, 0, 0))
    sheen_weight = max(sheen)
    shader.inputs["Sheen Weight"].default_value = sheen_weight
    shader.inputs["Sheen Roughness"].default_value = range_.get("sheenRoughness", 0)
    shader.inputs["Transmission Weight"].default_value = range_.get("transmission", 0)
    shader.inputs["IOR"].default_value = range_.get("ior", 1.5)
    if sheen_weight > 0:
        shader.inputs["Sheen Tint"].default_value = (*[channel / sheen_weight for channel in sheen], 1)
    if textures[2] >= 0:
        packed = nodes.new("ShaderNodeTexImage")
        packed.image = texture(textures[2], "Non-Color")
        channels = nodes.new("ShaderNodeSeparateColor")
        links.new(output_of(packed, "Color"), channels.inputs[0])
        for channel, socket, factor in (("Green", "Roughness", roughness), ("Blue", "Metallic", metallic)):
            scale = nodes.new("ShaderNodeMath")
            scale.operation = "MULTIPLY"
            scale.inputs[1].default_value = factor
            links.new(channels.outputs[channel], scale.inputs[0])
            links.new(scale.outputs[0], shader.inputs[socket])
    if textures[1] >= 0:
        normal_image = nodes.new("ShaderNodeTexImage")
        normal_image.image = texture(textures[1], "Non-Color")
        normal_map = nodes.new("ShaderNodeNormalMap")
        normal_map.uv_map = "UV"
        links.new(output_of(normal_image, "Color"), normal_map.inputs["Color"])
        links.new(normal_map.outputs["Normal"], shader.inputs["Normal"])
    emissive = range_["emissive"]
    if max(emissive) > 0:
        emission = list(emissive) + [1]
        if textures[4] >= 0:
            emission_image = nodes.new("ShaderNodeTexImage")
            emission_image.image = texture(textures[4], "sRGB")
            tint = nodes.new("ShaderNodeMix")
            tint.data_type = "RGBA"
            tint.blend_type = "MULTIPLY"
            input_of(tint, "Factor_Float").default_value = 1
            input_of(tint, "A_Color").default_value = emission
            links.new(output_of(emission_image, "Color"), input_of(tint, "B_Color"))
            links.new(output_of(tint, "Result_Color"), shader.inputs["Emission Color"])
        else:
            shader.inputs["Emission Color"].default_value = emission
        shader.inputs["Emission Strength"].default_value = 1
    return material


for number, range_ in enumerate(description["ranges"]):
    chunk = data[range_["start"]:range_["start"] + range_["count"]]
    count = len(chunk) - len(chunk) % 3
    if count == 0:
        continue
    chunk = chunk[:count]
    mesh = bpy.data.meshes.new("Range %d" % number)
    mesh.from_pydata(chunk[:, 0:3].tolist(), [], [(i, i + 1, i + 2) for i in range(0, count, 3)])
    colors = numpy.ones((count, 4), dtype=numpy.float32)
    colors[:, 0:3] = chunk[:, 6:9]
    attribute = mesh.color_attributes.new("Color", "FLOAT_COLOR", "CORNER")
    attribute.data.foreach_set("color", colors.ravel())
    uv = numpy.empty((count, 2), dtype=numpy.float32)
    uv[:, 0] = chunk[:, 9]
    uv[:, 1] = 1 - chunk[:, 10]
    mesh.uv_layers.new(name="UV").data.foreach_set("uv", uv.ravel())
    mesh.polygons.foreach_set("use_smooth", [True] * (count // 3))
    normals = chunk[:, 3:6].astype(numpy.float64)
    lengths = numpy.linalg.norm(normals, axis=1, keepdims=True)
    normals = numpy.where(lengths > 1e-8, normals / numpy.maximum(lengths, 1e-8), [0, 0, 1])
    mesh.normals_split_custom_set(normals.tolist())
    mesh.validate(clean_customdata=False)
    mesh.update()
    mesh.materials.append(make_material("Material %d" % number, range_))
    item = bpy.data.objects.new(mesh.name, mesh)
    scene.collection.objects.link(item)

# The sky the engine shows (Engine/Render/Shaders/Sky.frag without fog), as the world light.
world = bpy.data.worlds.new("Reference Sky")
scene.world = world
if world.node_tree is None:
    world.use_nodes = True
nodes, links = world.node_tree.nodes, world.node_tree.links
background = nodes["Background"]
sky = description["sky"]
sun = description["sun"]
sun_direction = Vector(sun["direction"]).normalized()
if sky["visible"] and sky.get("physical"):
    # The physical atmosphere, baked by the Player into an equirectangular radiance map.
    environment = nodes.new("ShaderNodeTexEnvironment")
    environment.image = bpy.data.images.load(os.path.join(folder, sky["map"]))
    environment.image.colorspace_settings.name = "Non-Color"
    links.new(environment.outputs["Color"], background.inputs["Color"])
    background.inputs["Strength"].default_value = 1
elif sky["visible"]:
    horizon, zenith = linear(sky["horizon"]), linear(sky["zenith"])
    coordinates = nodes.new("ShaderNodeTexCoord")
    direction = nodes.new("ShaderNodeVectorMath")
    direction.operation = "NORMALIZE"
    links.new(coordinates.outputs["Generated"], direction.inputs[0])
    axes = nodes.new("ShaderNodeSeparateXYZ")
    links.new(direction.outputs["Vector"], axes.inputs[0])

    def math_node(operation, value=None, second=None):
        node = nodes.new("ShaderNodeMath")
        node.operation = operation
        links.new(value, node.inputs[0])
        if second is not None:
            node.inputs[1].default_value = second
        return node.outputs[0]

    height = math_node("POWER", math_node("MAXIMUM", axes.outputs["Z"], 0.0), 0.55)
    gradient = nodes.new("ShaderNodeMix")
    gradient.data_type = "RGBA"
    links.new(height, input_of(gradient, "Factor_Float"))
    input_of(gradient, "A_Color").default_value = horizon + [1]
    input_of(gradient, "B_Color").default_value = zenith + [1]
    below = nodes.new("ShaderNodeMapRange")
    below.interpolation_type = "SMOOTHSTEP"
    below.inputs["From Min"].default_value = 0
    below.inputs["From Max"].default_value = 0.8
    links.new(math_node("MULTIPLY", axes.outputs["Z"], -1.0), below.inputs["Value"])
    ground = nodes.new("ShaderNodeMix")
    ground.data_type = "RGBA"
    links.new(below.outputs["Result"], input_of(ground, "Factor_Float"))
    links.new(output_of(gradient, "Result_Color"), input_of(ground, "A_Color"))
    input_of(ground, "B_Color").default_value = [h * g for h, g in zip(horizon, linear([0.67, 0.74, 0.80]))] + [1]
    toward = nodes.new("ShaderNodeVectorMath")
    toward.operation = "DOT_PRODUCT"
    links.new(direction.outputs["Vector"], toward.inputs[0])
    toward.inputs[1].default_value = sun_direction
    glow = math_node("POWER", math_node("MAXIMUM", toward.outputs["Value"], 0.0), 32.0)
    scaled = nodes.new("ShaderNodeVectorMath")
    scaled.operation = "SCALE"
    scaled.inputs[0].default_value = [sun["intensity"] * c for c in linear([0.42, 0.25, 0.12])]
    links.new(glow, scaled.inputs["Scale"])
    total = nodes.new("ShaderNodeVectorMath")
    total.operation = "ADD"
    links.new(output_of(ground, "Result_Color"), total.inputs[0])
    links.new(scaled.outputs["Vector"], total.inputs[1])
    links.new(total.outputs["Vector"], background.inputs["Color"])
    background.inputs["Strength"].default_value = 1
else:
    background.inputs["Strength"].default_value = 0


def add_light(name, kind, intensity, vector, shadows=True, color=(1, 1, 1)):
    light = bpy.data.lights.new(name, kind)
    light.color = color
    light.use_shadow = shadows
    item = bpy.data.objects.new(name, light)
    scene.collection.objects.link(item)
    if kind == "SUN":
        light.energy = math.pi * intensity
        light.angle = math.radians(0.53)
        item.rotation_euler = (-Vector(vector).normalized()).to_track_quat("-Z", "Y").to_euler()
    else:
        light.energy = 4 * math.pi * math.pi * intensity
        light.shadow_soft_size = 0.05
        item.location = vector
    return item


if sun["intensity"] > 0:
    add_light("Sun", "SUN", sun["intensity"], sun["direction"], sun["shadows"], tuple(sun.get("color", (1, 1, 1))))
for number, light in enumerate(description["lights"]):
    spot = light.get("spot")
    kind = "SUN" if light["directional"] else "SPOT" if spot else "POINT"
    item = add_light("Light %d" % number, kind, light["intensity"], light["vector"])
    item.data.color = light.get("color", (1, 1, 1))
    if spot:
        # Blender's spot light has the point light's intensity on its axis, inside a cone along its -Z axis.
        item.data.spot_size = spot["angle"]
        item.data.spot_blend = spot["blend"]
        item.rotation_euler = Vector(spot["direction"]).normalized().to_track_quat("-Z", "Y").to_euler()

camera_settings = description["camera"]
camera_data = bpy.data.cameras.new("Camera")
camera_data.sensor_fit = "VERTICAL"
camera_data.angle_y = camera_settings["verticalFov"]
camera_data.clip_start = camera_settings["near"]
camera_data.clip_end = camera_settings["far"]
camera = bpy.data.objects.new("Camera", camera_data)
scene.collection.objects.link(camera)
eye, target = Vector(camera_settings["eye"]), Vector(camera_settings["target"])
camera.location = eye
camera.rotation_euler = (target - eye).to_track_quat("-Z", "Y").to_euler()
scene.camera = camera

render = scene.render
render.engine = "CYCLES"
render.resolution_x = camera_settings["width"]
render.resolution_y = camera_settings["height"]
render.resolution_percentage = 100
render.film_transparent = False
render.image_settings.file_format = "PNG"
render.image_settings.color_mode = "RGB"
render.image_settings.color_depth = "8"
render.filepath = os.path.join(folder, "cycles.png")
cycles = scene.cycles
cycles.device = "CPU"
cycles.samples = samples
cycles.use_adaptive_sampling = True
cycles.adaptive_threshold = 0.01
cycles.seed = 0
cycles.max_bounces = 8
# WORLDFORGE_CYCLES_DIFFUSE_BOUNCES overrides the diffuse bounces, to measure how much of a view is bounced light.
cycles.diffuse_bounces = int(os.environ.get("WORLDFORGE_CYCLES_DIFFUSE_BOUNCES", "4"))
cycles.glossy_bounces = 4
cycles.transmission_bounces = 4
cycles.transparent_max_bounces = 8
cycles.blur_glossy = 1.0
denoised = bool(getattr(_cycles, "with_openimagedenoise", False))
cycles.use_denoising = denoised
if denoised:
    cycles.denoiser = "OPENIMAGEDENOISE"

display = description["display"]
scene.display_settings.display_device = "sRGB"
scene.view_settings.view_transform = display["viewTransform"]
scene.view_settings.look = "None"
scene.view_settings.exposure = display["exposure"]
scene.view_settings.gamma = 1

started = time.perf_counter()
bpy.ops.render.render(write_still=True)
seconds = time.perf_counter() - started
with open(os.path.join(folder, "cycles.json"), "w", encoding="utf-8") as file:
    json.dump({"seconds": round(seconds, 2), "samples": samples, "denoised": denoised,
               "version": bpy.app.version_string}, file)
print("WorldForge Cycles reference: %.1f s, %d samples%s" % (seconds, samples, ", denoised" if denoised else ""))
