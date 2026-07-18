#include "bridge-protocol.h"

#include <errno.h>
#include <gst/gst.h>
#include <gst/video/gstvideodecoder.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#ifndef PACKAGE
#define PACKAGE "tensor-g1-proot-gpu"
#endif

typedef struct _GstTensorH264Dec {
   GstVideoDecoder parent;
   int fd;
   guint width;
   guint height;
   guint stride;
   guint slice_height;
   guint fps_n;
   guint fps_d;
   guint64 decoded_frames;
   GstVideoCodecState *output_state;
} GstTensorH264Dec;

typedef struct _GstTensorH264DecClass {
   GstVideoDecoderClass parent_class;
} GstTensorH264DecClass;

#define GST_TYPE_TENSOR_H264_DEC (gst_tensor_h264_dec_get_type())
G_DEFINE_TYPE(GstTensorH264Dec, gst_tensor_h264_dec, GST_TYPE_VIDEO_DECODER)

GST_DEBUG_CATEGORY_STATIC(tensor_h264_dec_debug);
#define GST_CAT_DEFAULT tensor_h264_dec_debug

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
   "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
   GST_STATIC_CAPS("video/x-h264, stream-format=(string)byte-stream, "
                   "alignment=(string)au, width=(int)[1,MAX], "
                   "height=(int)[1,MAX]"));

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
   "src", GST_PAD_SRC, GST_PAD_ALWAYS,
   GST_STATIC_CAPS("video/x-raw, format=(string)NV12"));

static gboolean
transfer_bytes(int fd, void *data, gsize size, gboolean writing)
{
   guint8 *cursor = data;
   while (size) {
      ssize_t count =
         writing ? write(fd, cursor, size) : read(fd, cursor, size);
      if (count == 0)
         return FALSE;
      if (count < 0) {
         if (errno == EINTR)
            continue;
         return FALSE;
      }
      cursor += count;
      size -= (gsize)count;
   }
   return TRUE;
}

static gboolean
send_message(GstTensorH264Dec *self, guint16 type, gint64 pts_us,
             guint32 arg0, guint32 arg1, guint32 arg2, const void *payload,
             guint32 payload_size)
{
   struct tmc_message message = {
      .magic = TMC_MAGIC,
      .version = TMC_VERSION,
      .type = type,
      .payload_size = payload_size,
      .pts_us = pts_us,
      .arg0 = arg0,
      .arg1 = arg1,
      .arg2 = arg2,
   };
   return transfer_bytes(self->fd, &message, sizeof(message), TRUE) &&
          (!payload_size ||
           transfer_bytes(self->fd, (void *)payload, payload_size, TRUE));
}

static gboolean
receive_message(GstTensorH264Dec *self, struct tmc_message *message,
                guint8 **payload)
{
   *payload = NULL;
   if (!transfer_bytes(self->fd, message, sizeof(*message), FALSE) ||
       message->magic != TMC_MAGIC || message->version != TMC_VERSION ||
       message->payload_size > TMC_MAX_PAYLOAD)
      return FALSE;

   if (message->payload_size) {
      *payload = g_malloc(message->payload_size + 1u);
      if (!transfer_bytes(self->fd, *payload, message->payload_size, FALSE)) {
         g_free(*payload);
         *payload = NULL;
         return FALSE;
      }
      (*payload)[message->payload_size] = 0;
   }

   if (message->type == TMC_ERROR) {
      GST_ELEMENT_ERROR(self, STREAM, DECODE,
                        ("Android MediaCodec service rejected the stream"),
                        ("%s", *payload ? (char *)*payload : "unknown error"));
      g_clear_pointer(payload, g_free);
      return FALSE;
   }
   return TRUE;
}

static gboolean
connect_service(GstTensorH264Dec *self)
{
   const char *path = g_getenv("TENSOR_MEDIACODEC_SOCKET");
   if (!path || !*path)
      path = "/tmp/tensor-mediacodec.sock";

   struct sockaddr_un address = {.sun_family = AF_UNIX};
   if (strlen(path) >= sizeof(address.sun_path)) {
      GST_ELEMENT_ERROR(self, RESOURCE, OPEN_READ,
                        ("MediaCodec socket path is too long"), ("%s", path));
      return FALSE;
   }
   strcpy(address.sun_path, path);

   self->fd = socket(AF_UNIX, SOCK_STREAM, 0);
   if (self->fd < 0 ||
       connect(self->fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
      GST_ELEMENT_ERROR(self, RESOURCE, OPEN_READ,
                        ("Cannot connect to the Android MediaCodec service"),
                        ("%s: %s", path, g_strerror(errno)));
      if (self->fd >= 0)
         close(self->fd);
      self->fd = -1;
      return FALSE;
   }
   return TRUE;
}

static GstVideoCodecFrame *
find_frame_for_pts(GstVideoDecoder *decoder, GstClockTime pts)
{
   GList *frames = gst_video_decoder_get_frames(decoder);
   GstVideoCodecFrame *match = NULL;
   GstClockTime best_delta = GST_CLOCK_TIME_NONE;
   for (GList *item = frames; item; item = item->next) {
      GstVideoCodecFrame *frame = item->data;
      if (!GST_CLOCK_TIME_IS_VALID(frame->pts))
         continue;
      GstClockTime delta =
         frame->pts > pts ? frame->pts - pts : pts - frame->pts;
      if (delta <= GST_MSECOND &&
          (!GST_CLOCK_TIME_IS_VALID(best_delta) || delta < best_delta)) {
         if (match)
            gst_video_codec_frame_unref(match);
         match = gst_video_codec_frame_ref(frame);
         best_delta = delta;
      }
   }
   g_list_free_full(frames, (GDestroyNotify)gst_video_codec_frame_unref);
   return match;
}

static gboolean
set_output_format(GstTensorH264Dec *self, const struct tmc_message *message)
{
   GstVideoDecoder *decoder = GST_VIDEO_DECODER(self);
   self->width = message->arg0;
   self->height = message->arg1;
   self->stride = message->arg2 ? message->arg2 : self->width;
   self->slice_height = message->arg3 ? message->arg3 : self->height;
   if (!self->width || !self->height || self->stride < self->width ||
       self->slice_height < self->height)
      return FALSE;

   if (self->output_state)
      gst_video_codec_state_unref(self->output_state);
   self->output_state = gst_video_decoder_set_output_state(
      decoder, GST_VIDEO_FORMAT_NV12, self->width, self->height, NULL);
   if (!self->output_state)
      return FALSE;
   GST_INFO_OBJECT(self, "MediaCodec output %ux%u stride=%u slice-height=%u",
                   self->width, self->height, self->stride,
                   self->slice_height);
   return gst_video_decoder_negotiate(decoder);
}

static GstFlowReturn
push_frame(GstTensorH264Dec *self, const struct tmc_message *message,
           const guint8 *payload)
{
   GstVideoDecoder *decoder = GST_VIDEO_DECODER(self);
   GstClockTime pts = message->pts_us >= 0
                         ? (GstClockTime)message->pts_us * GST_USECOND
                         : GST_CLOCK_TIME_NONE;
   GstVideoCodecFrame *frame = find_frame_for_pts(decoder, pts);
   if (!frame) {
      GST_WARNING_OBJECT(
         self, "no queued input frame for output PTS %" G_GINT64_FORMAT,
         message->pts_us);
      return GST_FLOW_OK;
   }

   gsize y_source_size = (gsize)self->stride * self->slice_height;
   gsize uv_source_rows = (self->slice_height + 1u) / 2u;
   gsize required = y_source_size + (gsize)self->stride * uv_source_rows;
   gsize output_size = (gsize)self->width * self->height * 3u / 2u;
   if (!self->output_state || message->payload_size < required) {
      gst_video_codec_frame_unref(frame);
      GST_ELEMENT_ERROR(self, STREAM, DECODE,
                        ("Invalid MediaCodec NV12 output buffer"),
                        ("received %u bytes, need at least %zu",
                         message->payload_size, required));
      return GST_FLOW_ERROR;
   }

   GstBuffer *output = gst_buffer_new_allocate(NULL, output_size, NULL);
   GstMapInfo map;
   if (!output || !gst_buffer_map(output, &map, GST_MAP_WRITE)) {
      if (output)
         gst_buffer_unref(output);
      gst_video_codec_frame_unref(frame);
      return GST_FLOW_ERROR;
   }

   for (guint row = 0; row < self->height; ++row)
      memcpy(map.data + (gsize)row * self->width,
             payload + (gsize)row * self->stride, self->width);
   guint8 *output_uv = map.data + (gsize)self->width * self->height;
   const guint8 *source_uv = payload + y_source_size;
   for (guint row = 0; row < self->height / 2u; ++row)
      memcpy(output_uv + (gsize)row * self->width,
             source_uv + (gsize)row * self->stride, self->width);
   gst_buffer_unmap(output, &map);

   frame->output_buffer = output;
   GstFlowReturn flow = gst_video_decoder_finish_frame(decoder, frame);
   if (flow == GST_FLOW_OK)
      self->decoded_frames++;
   return flow;
}

static GstFlowReturn
process_until(GstTensorH264Dec *self, guint16 terminal_type)
{
   GstFlowReturn flow = GST_FLOW_OK;
   for (;;) {
      struct tmc_message message;
      guint8 *payload = NULL;
      if (!receive_message(self, &message, &payload))
         return GST_FLOW_ERROR;
      if (message.type == TMC_FORMAT && !set_output_format(self, &message))
         flow = GST_FLOW_NOT_NEGOTIATED;
      else if (message.type == TMC_FRAME && flow == GST_FLOW_OK)
         flow = push_frame(self, &message, payload);
      gboolean done = message.type == terminal_type;
      g_free(payload);
      if (flow != GST_FLOW_OK || done)
         return flow;
   }
}

static gboolean
gst_tensor_h264_dec_start(GstVideoDecoder *decoder)
{
   GstTensorH264Dec *self = (GstTensorH264Dec *)decoder;
   self->fd = -1;
   self->width = self->height = self->stride = self->slice_height = 0;
   self->fps_n = 30;
   self->fps_d = 1;
   self->decoded_frames = 0;
   self->output_state = NULL;
   gst_video_decoder_set_packetized(decoder, TRUE);
   return TRUE;
}

static gboolean
gst_tensor_h264_dec_stop(GstVideoDecoder *decoder)
{
   GstTensorH264Dec *self = (GstTensorH264Dec *)decoder;
   GST_INFO_OBJECT(self, "delivered %" G_GUINT64_FORMAT " decoded frames",
                   self->decoded_frames);
   if (self->fd >= 0)
      close(self->fd);
   self->fd = -1;
   if (self->output_state)
      gst_video_codec_state_unref(self->output_state);
   self->output_state = NULL;
   return TRUE;
}

static gboolean
gst_tensor_h264_dec_set_format(GstVideoDecoder *decoder,
                               GstVideoCodecState *state)
{
   GstTensorH264Dec *self = (GstTensorH264Dec *)decoder;
   const GstStructure *structure = gst_caps_get_structure(state->caps, 0);
   gint width = 0, height = 0, fps_n = 30, fps_d = 1;
   if (!gst_structure_get_int(structure, "width", &width) ||
       !gst_structure_get_int(structure, "height", &height))
      return FALSE;
   gst_structure_get_fraction(structure, "framerate", &fps_n, &fps_d);
   if (width <= 0 || height <= 0 || fps_n <= 0 || fps_d <= 0)
      return FALSE;

   if (self->fd >= 0)
      close(self->fd);
   self->fd = -1;
   if (!connect_service(self))
      return FALSE;

   self->fps_n = (guint)fps_n;
   self->fps_d = (guint)fps_d;
   const char *component = g_getenv("TENSOR_MEDIACODEC_COMPONENT");
   if (!component || !*component)
      component = "c2.exynos.h264.decoder";
   if (!send_message(self, TMC_CONFIG, 0, (guint32)width, (guint32)height,
                     (guint32)(fps_n / fps_d), component,
                     (guint32)strlen(component) + 1u))
      return FALSE;

   struct tmc_message message;
   guint8 *payload = NULL;
   gboolean ok = receive_message(self, &message, &payload) &&
                 message.type == TMC_READY;
   g_free(payload);
   return ok;
}

static GstFlowReturn
gst_tensor_h264_dec_handle_frame(GstVideoDecoder *decoder,
                                 GstVideoCodecFrame *frame)
{
   GstTensorH264Dec *self = (GstTensorH264Dec *)decoder;
   GstMapInfo map;
   if (self->fd < 0 ||
       !gst_buffer_map(frame->input_buffer, &map, GST_MAP_READ))
      return GST_FLOW_ERROR;

   GstClockTime pts = frame->pts;
   if (!GST_CLOCK_TIME_IS_VALID(pts)) {
      pts = gst_util_uint64_scale(frame->system_frame_number * GST_SECOND,
                                  self->fps_d, self->fps_n);
      frame->pts = pts;
   }
   gboolean sent = map.size <= TMC_MAX_PAYLOAD &&
                   send_message(self, TMC_PACKET, pts / GST_USECOND, 0, 0, 0,
                                map.data, (guint32)map.size);
   gst_buffer_unmap(frame->input_buffer, &map);
   if (!sent)
      return GST_FLOW_ERROR;
   return process_until(self, TMC_ACK);
}

static GstFlowReturn
gst_tensor_h264_dec_finish(GstVideoDecoder *decoder)
{
   GstTensorH264Dec *self = (GstTensorH264Dec *)decoder;
   if (self->fd < 0)
      return GST_FLOW_OK;
   if (!send_message(self, TMC_INPUT_EOS, 0, 0, 0, 0, NULL, 0))
      return GST_FLOW_ERROR;
   return process_until(self, TMC_OUTPUT_EOS);
}

static void
gst_tensor_h264_dec_class_init(GstTensorH264DecClass *klass)
{
   GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
   GstVideoDecoderClass *decoder_class = GST_VIDEO_DECODER_CLASS(klass);
   gst_element_class_add_static_pad_template(element_class, &sink_template);
   gst_element_class_add_static_pad_template(element_class, &src_template);
   gst_element_class_set_static_metadata(
      element_class, "Tensor G1 MediaCodec H.264 decoder",
      "Codec/Decoder/Video",
      "Rootless bridge to Android's Tensor/Exynos MediaCodec decoder",
      "tensor-g1-proot-gpu contributors");
   decoder_class->start = gst_tensor_h264_dec_start;
   decoder_class->stop = gst_tensor_h264_dec_stop;
   decoder_class->set_format = gst_tensor_h264_dec_set_format;
   decoder_class->handle_frame = gst_tensor_h264_dec_handle_frame;
   decoder_class->finish = gst_tensor_h264_dec_finish;
}

static void
gst_tensor_h264_dec_init(GstTensorH264Dec *self)
{
   self->fd = -1;
}

static gboolean
plugin_init(GstPlugin *plugin)
{
   GST_DEBUG_CATEGORY_INIT(tensor_h264_dec_debug, "tensorh264dec", 0,
                           "Tensor G1 MediaCodec bridge decoder");
   return gst_element_register(plugin, "tensorh264dec", GST_RANK_PRIMARY + 100,
                               GST_TYPE_TENSOR_H264_DEC);
}

GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, tensormediacodec,
                  "Tensor G1 Android MediaCodec bridge", plugin_init, "0.1.0",
                  "MIT", PACKAGE,
                  "https://github.com/SaicharanKandukuri/tensor-g1-proot-gpu")
