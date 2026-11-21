# Fits WorldForge's AgX approximation (Engine/Render/ColorPipeline.h) to Studio's AgX view.
# Run: WorldForgeStudio.exe -b --factory-startup --python Tests/FitAgx.py
#
# Model, applied to linear Rec.709 input c:
#   x = inset * c
#   e = clamp((log2(x) - lowStops) / (highStops - lowStops), 0, 1)
#   d = outset * pow(curve(e), 2.4)          (curve is a 129-point table)
#   display = sRGB encode(clamp(d, 0, 1))
import bpy
import numpy as np
import os
import tempfile

LOW, HIGH = -12.47393, 4.026069


def measure(samples):
    image = bpy.data.images.new("agx-fit", len(samples), 1, alpha=True, float_buffer=True)
    for name in ("Linear Rec.709", "Linear"):
        try:
            image.colorspace_settings.name = name
            break
        except TypeError:
            pass
    image.pixels = [v for s in samples for v in (*s, 1.0)]
    scene = bpy.context.scene
    scene.display_settings.display_device = "sRGB"
    view = scene.view_settings
    view.view_transform, view.look, view.exposure, view.gamma = "AgX", "None", 0, 1
    settings = scene.render.image_settings
    settings.file_format, settings.color_depth, settings.color_mode = "PNG", "16", "RGB"
    path = os.path.join(tempfile.gettempdir(), "worldforge-agx-fit.png")
    image.save_render(path, scene=scene)
    result = bpy.data.images.load(path)
    result.colorspace_settings.name = "Non-Color"
    pixels = np.array(result.pixels[:]).reshape(-1, result.channels)[:, :3]
    bpy.data.images.remove(result)
    bpy.data.images.remove(image)
    return pixels


def srgb_decode(v):
    return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4)


def srgb_encode(v):
    v = np.clip(v, 0, 1)
    return np.where(v <= 0.0031308, v * 12.92, 1.055 * v ** (1 / 2.4) - 0.055)


def normalized(x):
    return np.clip((np.log2(np.maximum(x, 1e-10)) - LOW) / (HIGH - LOW), 0, 1)


# Grey ramp: measure the curve at 129 evenly spaced points of the normalized log range.
curve_points = np.linspace(0, 1, 129)
greys = 2.0 ** (LOW + curve_points * (HIGH - LOW))
grey_out = measure([(g, g, g) for g in greys])[:, 1]
curve_table = srgb_decode(grey_out) ** (1 / 2.4)


def curve(e):
    return np.interp(e, curve_points, curve_table)


# Check the table between its points.
check = 0.18 * 2.0 ** (np.arange(-192, 97) / 16.0)
check_out = measure([(g, g, g) for g in check])[:, 1]

# Colors: fit inset and outset with neutral-preserving rows (each row sums to 1).
rng = np.random.default_rng(7)
colors = []
for _ in range(900):
    c = rng.random(3) ** 2
    c /= max(c.max(), 1e-3)
    colors.append(tuple(c * 0.18 * 2.0 ** rng.uniform(-7, 4)))
for base in [(1, 0, 0), (0, 1, 0), (0, 0, 1), (1, 1, 0), (0, 1, 1), (1, 0, 1)]:
    for stops in np.linspace(-7, 4, 23):
        colors.append(tuple(np.array(base) * 0.18 * 2.0 ** stops))
colors = np.array(colors)
color_out = measure([tuple(c) for c in colors])


def matrices(p):
    def build(q):
        m = np.array([[0, q[0], q[1]], [q[2], 0, q[3]], [q[4], q[5], 0]], dtype=float)
        np.fill_diagonal(m, 1 - m.sum(axis=1))
        return m
    return build(p[:6]), build(p[6:])


def model(p, c):
    inset, outset = matrices(p)
    x = c @ inset.T
    s = np.clip(curve(normalized(x)), 0, 1) ** 2.4
    return srgb_encode(s @ outset.T)


def residual(p):
    return (model(p, colors) - color_out).ravel()


p = np.zeros(12)
for iteration in range(60):
    r = residual(p)
    jacobian = np.empty((r.size, p.size))
    for i in range(p.size):
        step = np.zeros_like(p)
        step[i] = 1e-5
        jacobian[:, i] = (residual(p + step) - r) / 1e-5
    delta = np.linalg.lstsq(jacobian.T @ jacobian + 1e-6 * np.eye(p.size), -jacobian.T @ r, rcond=None)[0]
    p += delta
    if np.abs(delta).max() < 1e-9:
        break

inset, outset = matrices(p)
error = np.abs(model(p, colors) - color_out) * 255
grey_error = np.abs(srgb_encode(np.clip(curve(normalized(check)), 0, 1) ** 2.4) - check_out) * 255
print("AGX_FIT curve", ", ".join("%.7f" % v for v in curve_table))
print("AGX_FIT inset", ", ".join("%.9g" % v for v in inset.ravel()))
print("AGX_FIT outset", ", ".join("%.9g" % v for v in outset.ravel()))
print("AGX_FIT grey max/mean 8-bit error %.3f %.3f" % (grey_error.max(), grey_error.mean()))
print("AGX_FIT color max/mean/p99 8-bit error %.3f %.3f %.3f"
      % (error.max(), error.mean(), np.percentile(error, 99)))
