# SPDX-License-Identifier: MIT

"""Load an optional scene and disable overlays before the first redraw."""

import os

import bpy


scene_path = os.environ.get("TENSOR_BLEND_FILE")
if scene_path:
    bpy.ops.wm.open_mainfile(filepath=scene_path)

disabled = 0
for screen in bpy.data.screens:
    for area in screen.areas:
        if area.type != "VIEW_3D":
            continue

        area.spaces.active.overlay.show_overlays = False
        disabled += 1

print(f"TENSOR_G1_DISABLED_VIEWPORT_OVERLAYS={disabled}")

hidden_armatures = 0
if os.environ.get("TENSOR_HIDE_ARMATURES") == "1":
    for obj in bpy.data.objects:
        if obj.type != "ARMATURE":
            continue

        obj.hide_viewport = True
        hidden_armatures += 1

print(f"TENSOR_G1_HIDDEN_VIEWPORT_ARMATURES={hidden_armatures}")

output_path = os.environ.get("TENSOR_BLEND_OUTPUT")
if output_path:
    bpy.ops.wm.save_as_mainfile(filepath=output_path)
    print(f"TENSOR_G1_SAVED_SCENE={output_path}")
