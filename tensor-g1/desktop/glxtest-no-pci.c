/*
 * Firefox's Linux GL probe dlopens libpci whenever /sys/bus/pci exists.
 * Android exposes that directory but PRoot does not expose /proc/bus/pci, so
 * libpci terminates the short-lived probe before it can test EGL or GLX.
 *
 * This deliberately empty, Firefox-only shim makes the probe's dlsym checks
 * report "libpci missing methods". The probe treats that as a warning and
 * continues to the real graphics test. It must not be installed system-wide.
 */
void
tensor_firefox_glxtest_no_pci(void)
{
}
