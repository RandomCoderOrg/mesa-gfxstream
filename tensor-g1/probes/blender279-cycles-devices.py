# SPDX-License-Identifier: MIT

"""Report the Cycles compute devices visible to Blender 2.79."""

import json

import bpy


addon = bpy.context.user_preferences.addons.get("cycles")
result = {"addon_loaded": addon is not None, "devices": []}

if addon is not None:
    preferences = addon.preferences
    try:
        preferences.get_devices()
    except Exception as error:  # Blender reports unavailable backends here.
        result["enumeration_error"] = repr(error)

    result["compute_device_type"] = getattr(
        preferences, "compute_device_type", None
    )
    result["devices"] = [
        {
            "name": getattr(device, "name", ""),
            "type": getattr(device, "type", ""),
            "use": bool(getattr(device, "use", False)),
        }
        for device in getattr(preferences, "devices", [])
    ]

print("TENSOR_G1_CYCLES=" + json.dumps(result, sort_keys=True))
