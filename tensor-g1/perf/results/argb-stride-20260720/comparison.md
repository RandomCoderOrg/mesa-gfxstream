# Imported ARGB stride isolation

| Surface | Format | Imported size | Row stride | Stride mod 64 | Result |
| --- | --- | ---: | ---: | ---: | --- |
| Minimal aligned probe, 2240 px | BGRA8 | 2,555,904 B | 8,960 B | 0 | Pass |
| Minimal unaligned probe, 2248 px | BGRA8 | 2,564,096 B | 8,992 B | 32 | GPU fault `0x5b` |
| Plasma full-screen source, 2264 px | BGRA8 | 8,540,160 B | 9,056 B | 32 | GPU fault `0x5b` |
| KWin full-screen output, 2264 px | BGRX8 | 8,638,464 B | 9,088 B | 0 | Valid destination |

Both probe allocations exceeded their computed layout size. The only changed
variable in the minimal pair was an eight-pixel width increase, which changed
the four-byte pixel stride from 64-byte aligned to 32 bytes past alignment.
KWin owned `_NET_WM_CM_S0` in both runs. The aligned surface completed; the
unaligned surface reproduced Plasma's full-screen fragment-job fault without
starting Plasma Shell.

The historical Mesa change `811f8a19469722bea32f3c539b8cf0939fe3b057`
documents that regular formats on Panfrost v7 and newer require 64-byte row
strides and that violating the rule can raise an imprecise GPU fault. Mesa
later reverted the broad import rejection in
`4b19725ee525f6f0b5785436680cea63a21445a1` because rejecting such imports
caused compatibility regressions. For this rootless winsys the next experiment
is therefore an aligned staging resource for incompatible X pixmaps, not
rounding the descriptor stride while leaving the source tightly packed.
