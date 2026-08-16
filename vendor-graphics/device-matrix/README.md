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
