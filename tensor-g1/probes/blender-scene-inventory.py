# SPDX-License-Identifier: MIT

"""Print a compact Blender scene inventory for application-level GPU probes."""

import json

import bpy


particle_systems = [
    {
        "object": obj.name,
        "name": system.name,
        "type": system.settings.type,
        "render_type": system.settings.render_type,
        "count": system.settings.count,
    }
    for obj in bpy.data.objects
    for system in obj.particle_systems
]

print(
    "TENSOR_G1_SCENE="
    + json.dumps(
        {
            "scenes": [
                {"name": scene.name, "engine": scene.render.engine}
                for scene in bpy.data.scenes
            ],
            "objects": len(bpy.data.objects),
            "particle_systems": particle_systems,
        },
        sort_keys=True,
    )
)
