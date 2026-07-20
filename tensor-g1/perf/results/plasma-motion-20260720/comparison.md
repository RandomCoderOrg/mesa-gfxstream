# Plasma window-motion comparison

| Configuration | Displayed FPS median | Range | CPU cores | Thermal |
| --- | ---: | ---: | ---: | --- |
| CPU presenter + sync | 12.84 | 12.31-13.31 | 0.774 | 1, 1, 1, 1, 1 |
| CPU presenter + batchsync | 14.15 | 10.87-17.00 | 0.801 | 2, 2, 2, 2, 2 |
| No KWin compositor | 61.47 | 42.09-62.50 | 0.610 | 2, 2, 2, 2, 2 |
| DMA-BUF/DRI3 + sync | **FAIL** | KWin SIGBUS | - | - |
| DMA-BUF/DRI3 + batchsync | **FAIL** | KWin SIGBUS | - | - |

Displayed FPS comes from Android SurfaceFlinger TimeStats for the Termux:X11 SurfaceView. Each working configuration has one warm-up and five measured 120-move runs at a 60 Hz request rate.
