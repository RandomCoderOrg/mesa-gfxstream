# SPDX-License-Identifier: MIT

"""Render a bounded Eevee frame from the Blender hair regression scene."""

import os
import time

import bpy


scene_name = os.environ.get("TENSOR_BLEND_SCENE", "Scene_cat")
output_path = os.environ.get(
    "TENSOR_BLEND_OUTPUT", "/root/blender-scenes/fishy_cat-eevee-smoke.png"
)
size = int(os.environ.get("TENSOR_BLEND_SIZE", "256"))
samples = int(os.environ.get("TENSOR_BLEND_SAMPLES", "4"))

scene = bpy.data.scenes[scene_name]
scene.render.engine = "BLENDER_EEVEE"
scene.render.resolution_x = size
scene.render.resolution_y = size
scene.render.resolution_percentage = 100
scene.render.image_settings.file_format = "PNG"
scene.render.filepath = output_path
scene.eevee.taa_render_samples = samples

begin = time.monotonic()
bpy.ops.render.render(write_still=True, scene=scene.name)
elapsed = time.monotonic() - begin

print(
    "TENSOR_G1_RENDER="
    f"scene={scene.name} engine={scene.render.engine} size={size} "
    f"samples={samples} seconds={elapsed:.3f} output={output_path}"
)
