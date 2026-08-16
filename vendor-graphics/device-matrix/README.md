# Device matrix

Generated baseline JSON files live here. A baseline describes discovery and
permissions only; Vulkan, AHardwareBuffer, WSI, synchronization, Zink, and
performance results will be added as separate probe records.

Serial numbers are omitted from committed reports. The `deviceId` is derived
from stable public build properties so reports can be compared without
publishing an ADB identifier.

Every generated baseline must validate against `schema-v1.json`. A baseline is
immutable evidence for one Android build fingerprint; re-running after an OS
update produces a new `deviceId` and a separate qualification run.

WSI suite reports using the original lifecycle-only contract validate against
`wsi-qualification-schema-v1.json`. New reports validate against
`wsi-qualification-schema-v2.json`, which additionally requires observable
GPU Present-offload deltas. Keep a report separate from its discovery
baseline: the same Android build can be tested against multiple runtime
revisions without rewriting the original device evidence.
