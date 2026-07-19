// Dedicated experimental profile for Firefox on Tensor G1 PRoot.
user_pref("gfx.webrender.all", true);
user_pref("gfx.webrender.software", false);
// Keep Firefox on X11 EGL: stock Firefox requires EGL before it enables
// DMA-BUF and VA-API hardware decoding. This fork adds the reusable
// DMA-BUF/DRI3 presenter to Mesa's X11 EGL swrast loader.
user_pref("gfx.x11-egl.force-enabled", false);
user_pref("gfx.x11-egl.force-disabled", false);
user_pref("gfx.egl.prefer-gles.enabled", false);
user_pref("gfx.webrender.compositor", false);
user_pref("gfx.webrender.compositor.force-enabled", false);
user_pref("gfx.webrender.prefer-robustness", false);

// This preference only enables Firefox's Linux decode path. Hardware decode
// still needs a Tensor MediaCodec-to-VA-API/GStreamer bridge.
user_pref("media.ffmpeg.vaapi.enabled", true);

// The current Tensor VA-API frontend exposes H.264 only. Keep YouTube from
// preferring software-decoded VP9 or AV1 in this dedicated test profile so it
// requests an AVC stream that can reach Android MediaCodec.
user_pref("media.mediasource.vp9.enabled", false);
user_pref("media.av1.enabled", false);
