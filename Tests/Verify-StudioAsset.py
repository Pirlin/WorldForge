"""Exercise the Studio package codec and the editable Blender payload."""

import json
import importlib
from pathlib import Path
import shutil
import sys
import tempfile
from unittest.mock import patch
import zipfile

import bpy
import bmesh


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    arguments = sys.argv[sys.argv.index("--") + 1:]
    output = Path(arguments[1]) if len(arguments) > 1 else None
    operators = importlib.import_module("bl_operators")
    module_path = str(Path(arguments[0]).resolve())
    if module_path not in operators.__path__:
        operators.__path__.insert(0, module_path)
    from bl_operators.worldforge_asset import PackageError, _write_lock, read_package, write_package
    from bl_operators import worldforge_documents as documents

    for cls in documents.classes:
        if not cls.is_registered:
            bpy.utils.register_class(cls)
    documents.register_handlers()

    with tempfile.TemporaryDirectory() as directory:
        directory = Path(directory)
        bpy.ops.wm.read_factory_settings(use_empty=True)
        bpy.ops.mesh.primitive_cube_add()
        cube = bpy.context.object
        modifier = cube.modifiers.new("Editable bevel", "BEVEL")
        modifier.segments = 3
        material = bpy.data.materials.new("Fixture material")
        cube.data.materials.append(material)
        image = bpy.data.images.new("Fixture color", width=4, height=4)
        image.pixels[:] = [0.8, 0.2, 0.4, 1.0] * 16
        image.pack()
        texture = material.node_tree.nodes.new("ShaderNodeTexImage")
        texture.image = image
        shader = next(node for node in material.node_tree.nodes if node.type == "BSDF_PRINCIPLED")
        material.node_tree.links.new(texture.outputs["Color"], shader.inputs["Base Color"])
        bpy.ops.object.mode_set(mode="EDIT")
        mesh = bmesh.from_edit_mesh(cube.data)
        for vertex in mesh.verts:
            vertex.co.z += 0.25
        bmesh.update_edit_mesh(cube.data)
        bpy.ops.object.mode_set(mode="OBJECT")
        payload = directory / "source.blend"
        bpy.ops.wm.save_as_mainfile(filepath=str(payload), copy=True)
        asset = directory / "fixture.wfasset"
        original = write_package(asset, payload, bpy.app.version_string)
        moved = directory / "moved.wfasset"
        shutil.move(asset, moved)
        restored = directory / "restored.blend"
        require(read_package(moved, restored) == original, "Asset identity changed on move")
        bpy.ops.wm.open_mainfile(filepath=str(restored), use_scripts=False)
        reopened = bpy.data.objects["Cube"]
        require(reopened.modifiers["Editable bevel"].segments == 3, "Bevel was lost")
        require(bpy.data.images["Fixture color"].packed_file is not None, "Texture was lost")
        require(max(vertex.co.z for vertex in reopened.data.vertices) == 1.25, "Mesh edit was lost")
        revised = write_package(moved, payload, bpy.app.version_string, expected=original)
        require(revised["asset_id"] == original["asset_id"] and revised["revision"] == 2,
                "Revision did not preserve identity")
        try:
            write_package(moved, payload, bpy.app.version_string, expected=original)
        except PackageError:
            pass
        else:
            raise RuntimeError("Stale write was accepted")
        require(read_package(moved) == revised, "Stale write changed the valid package")

        with patch("os.replace", side_effect=OSError("Simulated interrupted replacement")):
            try:
                write_package(moved, payload, bpy.app.version_string, expected=revised)
            except OSError:
                pass
            else:
                raise RuntimeError("Interrupted replacement was treated as a successful save")
        require(read_package(moved) == revised, "Interrupted save changed the valid package")

        with _write_lock(moved):
            try:
                write_package(moved, payload, bpy.app.version_string, expected=revised)
            except PackageError as error:
                require("Another Studio session" in str(error), "Concurrent writer produced the wrong diagnostic")
            else:
                raise RuntimeError("Concurrent writer entered the active write lock")
        require(read_package(moved) == revised, "Concurrent writer changed the valid package")

        invalid = directory / "invalid.wfasset"
        with zipfile.ZipFile(invalid, "w") as archive:
            archive.writestr("manifest.json", json.dumps(revised))
            archive.writestr("source.blend", b"corrupt")
        try:
            read_package(invalid, restored)
        except PackageError:
            pass
        else:
            raise RuntimeError("Corrupt payload was accepted")
        require(restored.read_bytes() == payload.read_bytes(), "Corrupt package changed the last extraction")
        with zipfile.ZipFile(invalid, "w") as archive:
            archive.writestr("manifest.json", json.dumps(dict(revised, format_version=999)))
            archive.write(payload, "source.blend")
        try:
            read_package(invalid)
        except PackageError as error:
            require("unsupported format version" in str(error), "Newer format produced the wrong diagnostic")
        else:
            raise RuntimeError("Newer asset format was accepted")
        native = directory / "native.wfasset"
        require(bpy.ops.wm.worldforge_save_asset(filepath=str(native)) == {"FINISHED"},
                "Studio Save failed")
        first = read_package(native)
        require(bpy.ops.wm.worldforge_open_asset(filepath=str(native)) == {"FINISHED"},
                "Studio Open failed")
        require(documents.active_path() == native, "Studio lost the active asset path")
        renamed = directory / "renamed.wfasset"
        require(bpy.ops.wm.worldforge_save_asset(filepath=str(renamed), mode="SAVE_AS") == {"FINISHED"},
                "Studio Save As failed")
        require(read_package(renamed)["asset_id"] == first["asset_id"], "Save As changed the asset ID")
        copy = directory / "copy.wfasset"
        require(bpy.ops.wm.worldforge_save_asset(filepath=str(copy), mode="COPY") == {"FINISHED"},
                "Studio Save a Copy failed")
        require(read_package(copy)["asset_id"] != first["asset_id"], "Copy reused the asset ID")
        require(documents.active_path() == renamed, "Save a Copy changed the active path")
        documents.save_recovery()
        require(bpy.ops.wm.worldforge_recover_asset() == {"FINISHED"}, "Recovery failed")
        require(documents.active_path() is None, "Recovered work can overwrite its original")
        require(read_package(renamed)["revision"] == 2, "Recovery changed the published file")
        missing = bpy.data.images.new("Missing texture", width=4, height=4)
        missing.source = "FILE"
        missing.filepath = str(directory / "missing.png")
        blocked = directory / "unportable.wfasset"
        try:
            documents.save_asset(blocked)
        except (PackageError, RuntimeError) as error:
            require("missing.png" in str(error), "Missing dependency produced the wrong diagnostic")
        else:
            raise RuntimeError("A package with a missing texture was treated as portable")
        require(not blocked.exists(), "Failed save left an asset behind")
        if output is not None:
            shutil.copy2(renamed, output)
        bpy.ops.wm.read_factory_settings(use_empty=True)
    print("PASS: portable asset round trip, stale-write rejection, corruption, Save As, copy and recovery")


if __name__ == "__main__":
    main()
