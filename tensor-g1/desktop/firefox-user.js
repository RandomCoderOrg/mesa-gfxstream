// Dedicated experimental profile for Firefox on Tensor G1 PRoot.
user_pref("gfx.webrender.all", true);
user_pref("gfx.webrender.software", false);
// Let Firefox choose X11 EGL normally. The app-scoped libpci shim allows its
// graphics probe to reach EGL without forcing a backend preference here.
user_pref("gfx.x11-egl.force-enabled", false);
user_pref("gfx.x11-egl.force-disabled", false);
user_pref("gfx.egl.prefer-gles.enabled", false);
user_pref("gfx.webrender.compositor", false);
user_pref("gfx.webrender.compositor.force-enabled", false);
user_pref("gfx.webrender.prefer-robustness", false);

// This preference only enables Firefox's Linux decode path. Hardware decode
// still needs a Tensor MediaCodec-to-VA-API/GStreamer bridge.
user_pref("media.ffmpeg.vaapi.enabled", true);
