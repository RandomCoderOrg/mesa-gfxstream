#define _GNU_SOURCE

/*
 * Minimal VA-API frontend for the rootless Tensor MediaCodec bridge.
 *
 * This first stage deliberately advertises only the H.264 decode profiles
 * already implemented by mediacodec-service.  The resource and picture entry
 * points are present so libva can validate the driver; decode plumbing is
 * filled in incrementally rather than claiming codecs the bridge cannot run.
 */

#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <va/va_backend.h>
#include <va/va_drmcommon.h>

#include <drm/drm_fourcc.h>

#include "../bridge-protocol.h"

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif

#define TENSOR_MAX_CONFIGS 16
#define TENSOR_MAX_SURFACES 64
#define TENSOR_MAX_CONTEXTS 16
#define TENSOR_MAX_BUFFERS 256
#define TENSOR_MAX_IMAGES 64

struct tensor_va_config {
   bool used;
   VAProfile profile;
   VAEntrypoint entrypoint;
};

struct tensor_va_surface {
   bool used;
   unsigned width;
   unsigned height;
   unsigned stride;
   unsigned slice_height;
   int dma_fd;
   uint8_t *data;
   size_t data_size;
   size_t frame_size;
   int64_t pts_us;
   VASurfaceStatus status;
};

struct tensor_va_context {
   bool used;
   VAConfigID config_id;
   unsigned width;
   unsigned height;
   VASurfaceID current_target;
   unsigned submitted_frames;
   int socket_fd;
   uint8_t *slice_data;
   size_t slice_size;
   size_t slice_capacity;
   VAPictureParameterBufferH264 picture;
   VASliceParameterBufferH264 first_slice;
   bool have_picture;
   bool have_first_slice;
   unsigned pps_id;
   unsigned output_width;
   unsigned output_height;
   unsigned output_stride;
   unsigned output_slice_height;
};

struct tensor_va_buffer {
   bool used;
   VABufferType type;
   unsigned size;
   unsigned num_elements;
   void *data;
   bool owns_data;
};

struct tensor_va_image {
   bool used;
   VASurfaceID surface_id;
   VAImage image;
};

struct tensor_va_driver {
   struct tensor_va_config configs[TENSOR_MAX_CONFIGS];
   struct tensor_va_surface surfaces[TENSOR_MAX_SURFACES];
   struct tensor_va_context contexts[TENSOR_MAX_CONTEXTS];
   struct tensor_va_buffer buffers[TENSOR_MAX_BUFFERS];
   struct tensor_va_image images[TENSOR_MAX_IMAGES];
};

static struct tensor_va_config *
tensor_config(struct tensor_va_driver *driver, VAConfigID id)
{
   if (!id || id > TENSOR_MAX_CONFIGS || !driver->configs[id - 1].used)
      return NULL;
   return &driver->configs[id - 1];
}

static struct tensor_va_surface *
tensor_surface(struct tensor_va_driver *driver, VASurfaceID id)
{
   if (!id || id > TENSOR_MAX_SURFACES || !driver->surfaces[id - 1].used)
      return NULL;
   return &driver->surfaces[id - 1];
}

static struct tensor_va_context *
tensor_context(struct tensor_va_driver *driver, VAContextID id)
{
   if (!id || id > TENSOR_MAX_CONTEXTS || !driver->contexts[id - 1].used)
      return NULL;
   return &driver->contexts[id - 1];
}

static struct tensor_va_buffer *
tensor_buffer(struct tensor_va_driver *driver, VABufferID id)
{
   if (!id || id > TENSOR_MAX_BUFFERS || !driver->buffers[id - 1].used)
      return NULL;
   return &driver->buffers[id - 1];
}

static bool
tensor_profile_supported(VAProfile profile)
{
   return profile == VAProfileH264ConstrainedBaseline ||
          profile == VAProfileH264Main || profile == VAProfileH264High;
}

static bool
tensor_debug_enabled(void)
{
   const char *value = getenv("TENSOR_VA_DEBUG");
   return value && *value && strcmp(value, "0") != 0;
}

static bool
tensor_dma_buf_sync(int fd, uint64_t flags)
{
   struct dma_buf_sync sync = {.flags = flags};
   int result;

   do {
      result = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
   } while (result < 0 && errno == EINTR);

   if (result < 0 && tensor_debug_enabled()) {
      fprintf(stderr, "tensor-va: DMA_BUF_IOCTL_SYNC flags=0x%llx failed: %s\n",
              (unsigned long long)flags, strerror(errno));
   }
   return result == 0;
}

static bool
tensor_allocate_surface(struct tensor_va_surface *surface, unsigned width,
                        unsigned height)
{
   if (!width || !height || width > SIZE_MAX / height)
      return false;
   size_t pixels = (size_t)width * height;
   if (pixels > SIZE_MAX - pixels / 2)
      return false;
   size_t bytes = pixels + pixels / 2;
   long page_size = sysconf(_SC_PAGESIZE);
   if (page_size <= 0)
      page_size = 4096;
   size_t page = (size_t)page_size;
   if (bytes > SIZE_MAX - (page - 1))
      return false;
   size_t allocation_size = (bytes + page - 1) & ~(page - 1);

   const char *heap_name = getenv("TENSOR_VA_DMA_HEAP");
   if (!heap_name || !*heap_name)
      heap_name = "system";
   char heap_path[128];
   if (snprintf(heap_path, sizeof(heap_path), "/dev/dma_heap/%s", heap_name) >=
       (int)sizeof(heap_path))
      return false;

   int heap = open(heap_path, O_RDONLY | O_CLOEXEC);
   if (heap < 0)
      return false;
   struct dma_heap_allocation_data allocation = {
      .len = allocation_size,
      .fd_flags = O_RDWR | O_CLOEXEC,
   };
   int result = ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &allocation);
   close(heap);
   if (result < 0)
      return false;

   void *mapping = mmap(NULL, allocation_size, PROT_READ | PROT_WRITE,
                        MAP_SHARED, (int)allocation.fd, 0);
   if (mapping == MAP_FAILED) {
      close((int)allocation.fd);
      return false;
   }

   surface->width = width;
   surface->height = height;
   surface->stride = width;
   surface->slice_height = height;
   surface->dma_fd = (int)allocation.fd;
   surface->data = mapping;
   surface->data_size = allocation_size;
   return true;
}

static void
tensor_release_surface(struct tensor_va_surface *surface)
{
   if (surface->data && surface->data_size)
      munmap(surface->data, surface->data_size);
   if (surface->dma_fd >= 0)
      close(surface->dma_fd);
   memset(surface, 0, sizeof(*surface));
   surface->dma_fd = -1;
}

struct tensor_bit_writer {
   uint8_t data[1024];
   size_t bit_count;
   bool overflow;
};

struct tensor_bit_reader {
   uint8_t data[128];
   size_t bit_count;
   size_t bit_offset;
};

static bool
tensor_transfer(int fd, void *data, size_t size, bool writing)
{
   uint8_t *cursor = data;
   while (size) {
      ssize_t count = writing ? send(fd, cursor, size, MSG_NOSIGNAL)
                              : read(fd, cursor, size);
      if (count == 0)
         return false;
      if (count < 0) {
         if (errno == EINTR)
            continue;
         return false;
      }
      cursor += count;
      size -= (size_t)count;
   }
   return true;
}

static bool
tensor_send_message(int fd, uint16_t type, int64_t pts_us, uint32_t arg0,
                    uint32_t arg1, uint32_t arg2, const void *payload,
                    uint32_t payload_size)
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
   return tensor_transfer(fd, &message, sizeof(message), true) &&
          (!payload_size ||
           tensor_transfer(fd, (void *)payload, payload_size, true));
}

static bool
tensor_receive_message(int fd, struct tmc_message *message, uint8_t **payload)
{
   *payload = NULL;
   if (!tensor_transfer(fd, message, sizeof(*message), false) ||
       message->magic != TMC_MAGIC || message->version != TMC_VERSION ||
       message->payload_size > TMC_MAX_PAYLOAD)
      return false;

   if (message->payload_size) {
      *payload = malloc(message->payload_size + 1u);
      if (!*payload ||
          !tensor_transfer(fd, *payload, message->payload_size, false)) {
         free(*payload);
         *payload = NULL;
         return false;
      }
      (*payload)[message->payload_size] = 0;
   }
   if (message->type == TMC_ERROR) {
      fprintf(stderr, "tensor-va: MediaCodec service: %s\n",
              *payload ? (char *)*payload : "unknown error");
      free(*payload);
      *payload = NULL;
      return false;
   }
   return true;
}

static bool
tensor_append_bytes(uint8_t **data, size_t *size, size_t *capacity,
                    const void *source, size_t source_size)
{
   if (source_size > SIZE_MAX - *size)
      return false;
   size_t needed = *size + source_size;
   if (needed > *capacity) {
      size_t new_capacity = *capacity ? *capacity : 4096;
      while (new_capacity < needed) {
         if (new_capacity > SIZE_MAX / 2) {
            new_capacity = needed;
            break;
         }
         new_capacity *= 2;
      }
      uint8_t *resized = realloc(*data, new_capacity);
      if (!resized)
         return false;
      *data = resized;
      *capacity = new_capacity;
   }
   memcpy(*data + *size, source, source_size);
   *size = needed;
   return true;
}

static void
tensor_put_bit(struct tensor_bit_writer *writer, unsigned value)
{
   if (writer->bit_count >= sizeof(writer->data) * CHAR_BIT) {
      writer->overflow = true;
      return;
   }
   size_t byte = writer->bit_count / CHAR_BIT;
   unsigned shift = 7u - (unsigned)(writer->bit_count % CHAR_BIT);
   if (value & 1u)
      writer->data[byte] |= (uint8_t)(1u << shift);
   writer->bit_count++;
}

static void
tensor_put_bits(struct tensor_bit_writer *writer, uint32_t value,
                unsigned count)
{
   for (unsigned i = count; i > 0; i--)
      tensor_put_bit(writer, value >> (i - 1));
}

static void
tensor_put_ue(struct tensor_bit_writer *writer, unsigned value)
{
   uint64_t code = (uint64_t)value + 1u;
   unsigned bits = 0;
   for (uint64_t cursor = code; cursor; cursor >>= 1)
      bits++;
   for (unsigned i = 1; i < bits; i++)
      tensor_put_bit(writer, 0);
   for (unsigned i = bits; i > 0; i--)
      tensor_put_bit(writer, (unsigned)(code >> (i - 1)));
}

static void
tensor_put_se(struct tensor_bit_writer *writer, int value)
{
   unsigned code = value <= 0 ? (unsigned)(-(int64_t)value * 2)
                              : (unsigned)((int64_t)value * 2 - 1);
   tensor_put_ue(writer, code);
}

static size_t
tensor_finish_rbsp(struct tensor_bit_writer *writer)
{
   tensor_put_bit(writer, 1);
   while (writer->bit_count % CHAR_BIT)
      tensor_put_bit(writer, 0);
   return writer->overflow ? 0 : writer->bit_count / CHAR_BIT;
}

static bool
tensor_append_nal(uint8_t **packet, size_t *packet_size,
                  size_t *packet_capacity, uint8_t nal_header,
                  const uint8_t *rbsp, size_t rbsp_size)
{
   static const uint8_t start_code[] = {0, 0, 0, 1};
   if (!tensor_append_bytes(packet, packet_size, packet_capacity, start_code,
                            sizeof(start_code)) ||
       !tensor_append_bytes(packet, packet_size, packet_capacity, &nal_header,
                            1))
      return false;

   unsigned zeroes = 0;
   for (size_t i = 0; i < rbsp_size; i++) {
      if (zeroes >= 2 && rbsp[i] <= 3) {
         const uint8_t escape = 3;
         if (!tensor_append_bytes(packet, packet_size, packet_capacity,
                                  &escape, 1))
            return false;
         zeroes = 0;
      }
      if (!tensor_append_bytes(packet, packet_size, packet_capacity, &rbsp[i],
                               1))
         return false;
      zeroes = rbsp[i] == 0 ? zeroes + 1 : 0;
   }
   return true;
}

static bool
tensor_read_bit(struct tensor_bit_reader *reader, unsigned *value)
{
   if (reader->bit_offset >= reader->bit_count)
      return false;
   size_t byte = reader->bit_offset / CHAR_BIT;
   unsigned shift = 7u - (unsigned)(reader->bit_offset % CHAR_BIT);
   *value = (reader->data[byte] >> shift) & 1u;
   reader->bit_offset++;
   return true;
}

static bool
tensor_read_ue(struct tensor_bit_reader *reader, unsigned *value)
{
   unsigned leading_zeroes = 0;
   unsigned bit = 0;
   while (tensor_read_bit(reader, &bit) && !bit) {
      if (++leading_zeroes > 31)
         return false;
   }
   if (!bit)
      return false;
   uint32_t code = 1;
   for (unsigned i = 0; i < leading_zeroes; i++) {
      if (!tensor_read_bit(reader, &bit))
         return false;
      code = (code << 1) | bit;
   }
   *value = code - 1;
   return true;
}

static unsigned
tensor_slice_pps_id(const uint8_t *data, size_t size)
{
   struct tensor_bit_reader reader = {0};
   if (size < 2)
      return 0;

   unsigned zeroes = 0;
   for (size_t i = 1; i < size && reader.bit_count / CHAR_BIT <
                                      sizeof(reader.data);
        i++) {
      if (zeroes >= 2 && data[i] == 3) {
         zeroes = 0;
         continue;
      }
      size_t output = reader.bit_count / CHAR_BIT;
      reader.data[output] = data[i];
      reader.bit_count += CHAR_BIT;
      zeroes = data[i] == 0 ? zeroes + 1 : 0;
   }

   unsigned ignored = 0;
   unsigned pps_id = 0;
   if (!tensor_read_ue(&reader, &ignored) ||
       !tensor_read_ue(&reader, &ignored) ||
       !tensor_read_ue(&reader, &pps_id))
      return 0;
   return pps_id;
}

static bool
tensor_append_parameter_sets(struct tensor_va_driver *driver,
                             struct tensor_va_context *context,
                             uint8_t **packet, size_t *packet_size,
                             size_t *packet_capacity)
{
   struct tensor_va_config *config =
      tensor_config(driver, context->config_id);
   const VAPictureParameterBufferH264 *picture = &context->picture;
   const VASliceParameterBufferH264 *slice = &context->first_slice;
   if (!config || !context->have_picture || !context->have_first_slice)
      return false;

   unsigned profile_idc = config->profile == VAProfileH264High ? 100 :
                          config->profile == VAProfileH264Main ? 77 : 66;
   unsigned constraints =
      config->profile == VAProfileH264ConstrainedBaseline ? 0xc0 : 0;
   unsigned chroma_format = picture->seq_fields.bits.chroma_format_idc;
   if (!chroma_format)
      chroma_format = 1;

   struct tensor_bit_writer sps = {0};
   tensor_put_bits(&sps, profile_idc, 8);
   tensor_put_bits(&sps, constraints, 8);
   tensor_put_bits(&sps, 42, 8); /* Level 4.2 covers 1080p60 on Tensor G1. */
   tensor_put_ue(&sps, 0);       /* seq_parameter_set_id */
   if (profile_idc == 100) {
      tensor_put_ue(&sps, chroma_format);
      if (chroma_format == 3)
         tensor_put_bit(
            &sps,
            picture->seq_fields.bits.residual_colour_transform_flag);
      tensor_put_ue(&sps, picture->bit_depth_luma_minus8);
      tensor_put_ue(&sps, picture->bit_depth_chroma_minus8);
      tensor_put_bit(&sps, 0); /* qpprime_y_zero_transform_bypass_flag */
      tensor_put_bit(&sps, 0); /* seq_scaling_matrix_present_flag */
   }
   tensor_put_ue(&sps, picture->seq_fields.bits.log2_max_frame_num_minus4);
   tensor_put_ue(&sps, picture->seq_fields.bits.pic_order_cnt_type);
   if (picture->seq_fields.bits.pic_order_cnt_type == 0) {
      tensor_put_ue(
         &sps,
         picture->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4);
   } else if (picture->seq_fields.bits.pic_order_cnt_type == 1) {
      tensor_put_bit(
         &sps,
         picture->seq_fields.bits.delta_pic_order_always_zero_flag);
      tensor_put_se(&sps, 0);
      tensor_put_se(&sps, 0);
      tensor_put_ue(&sps, 0);
   }
   tensor_put_ue(&sps, picture->num_ref_frames);
   tensor_put_bit(
      &sps, picture->seq_fields.bits.gaps_in_frame_num_value_allowed_flag);
   tensor_put_ue(&sps, picture->picture_width_in_mbs_minus1);
   tensor_put_ue(&sps, picture->picture_height_in_mbs_minus1);
   tensor_put_bit(&sps, picture->seq_fields.bits.frame_mbs_only_flag);
   if (!picture->seq_fields.bits.frame_mbs_only_flag)
      tensor_put_bit(
         &sps, picture->seq_fields.bits.mb_adaptive_frame_field_flag);
   tensor_put_bit(&sps, picture->seq_fields.bits.direct_8x8_inference_flag);
   tensor_put_bit(&sps, 0); /* frame_cropping_flag: VA exposes coded size. */
   tensor_put_bit(&sps, 0); /* vui_parameters_present_flag */
   size_t sps_size = tensor_finish_rbsp(&sps);
   if (!sps_size ||
       !tensor_append_nal(packet, packet_size, packet_capacity, 0x67,
                          sps.data, sps_size))
      return false;

   struct tensor_bit_writer pps = {0};
   tensor_put_ue(&pps, context->pps_id);
   tensor_put_ue(&pps, 0); /* seq_parameter_set_id */
   tensor_put_bit(&pps, picture->pic_fields.bits.entropy_coding_mode_flag);
   tensor_put_bit(&pps, picture->pic_fields.bits.pic_order_present_flag);
   tensor_put_ue(&pps, 0); /* num_slice_groups_minus1 */
   tensor_put_ue(&pps, slice->num_ref_idx_l0_active_minus1);
   tensor_put_ue(&pps, slice->num_ref_idx_l1_active_minus1);
   tensor_put_bit(&pps, picture->pic_fields.bits.weighted_pred_flag);
   tensor_put_bits(&pps, picture->pic_fields.bits.weighted_bipred_idc, 2);
   tensor_put_se(&pps, picture->pic_init_qp_minus26);
   tensor_put_se(&pps, picture->pic_init_qs_minus26);
   tensor_put_se(&pps, picture->chroma_qp_index_offset);
   tensor_put_bit(
      &pps, picture->pic_fields.bits.deblocking_filter_control_present_flag);
   tensor_put_bit(&pps, picture->pic_fields.bits.constrained_intra_pred_flag);
   tensor_put_bit(&pps, picture->pic_fields.bits.redundant_pic_cnt_present_flag);
   if (profile_idc == 100) {
      tensor_put_bit(&pps, picture->pic_fields.bits.transform_8x8_mode_flag);
      tensor_put_bit(&pps, 0); /* pic_scaling_matrix_present_flag */
      tensor_put_se(&pps, picture->second_chroma_qp_index_offset);
   }
   size_t pps_size = tensor_finish_rbsp(&pps);
   return pps_size &&
          tensor_append_nal(packet, packet_size, packet_capacity, 0x68,
                            pps.data, pps_size);
}

static bool
tensor_store_frame(struct tensor_va_driver *driver,
                   struct tensor_va_context *context,
                   const struct tmc_message *message, const uint8_t *payload)
{
   for (unsigned i = 0; i < TENSOR_MAX_SURFACES; i++) {
      struct tensor_va_surface *surface = &driver->surfaces[i];
      if (!surface->used || surface->pts_us != message->pts_us)
         continue;
      if (message->payload_size > surface->data_size)
         return false;
      if (message->payload_size) {
         if (!tensor_dma_buf_sync(surface->dma_fd,
                                  DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE))
            return false;
         memcpy(surface->data, payload, message->payload_size);
         if (!tensor_dma_buf_sync(surface->dma_fd,
                                  DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE))
            return false;
      }
      surface->frame_size = message->payload_size;
      surface->stride = context->output_stride;
      surface->slice_height = context->output_slice_height;
      surface->status = VASurfaceReady;
      if (tensor_debug_enabled())
         fprintf(stderr,
                 "tensor-va: decoded pts=%lld bytes=%u stride=%u slice=%u\n",
                 (long long)message->pts_us, message->payload_size,
                 surface->stride, surface->slice_height);
      return true;
   }
   if (tensor_debug_enabled())
      fprintf(stderr, "tensor-va: dropped output for unknown pts=%lld\n",
              (long long)message->pts_us);
   return true;
}

static bool
tensor_process_until(struct tensor_va_driver *driver,
                     struct tensor_va_context *context, uint16_t terminal)
{
   for (;;) {
      struct tmc_message message;
      uint8_t *payload = NULL;
      if (!tensor_receive_message(context->socket_fd, &message, &payload))
         return false;
      bool ok = true;
      if (message.type == TMC_FORMAT) {
         context->output_width = message.arg0;
         context->output_height = message.arg1;
         context->output_stride = message.arg2 ? message.arg2 : message.arg0;
         context->output_slice_height =
            message.arg3 ? message.arg3 : message.arg1;
      } else if (message.type == TMC_FRAME) {
         ok = tensor_store_frame(driver, context, &message, payload);
      }
      free(payload);
      if (!ok)
         return false;
      if (message.type == terminal)
         return true;
   }
}

static bool
tensor_connect_service(struct tensor_va_driver *driver,
                       struct tensor_va_context *context)
{
   const char *path = getenv("TENSOR_MEDIACODEC_SOCKET");
   if (!path || !*path)
      path = "/tmp/tensor-mediacodec.sock";
   if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path))
      return false;

   context->socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
   struct sockaddr_un address = {.sun_family = AF_UNIX};
   strcpy(address.sun_path, path);
   if (context->socket_fd < 0 ||
       connect(context->socket_fd, (struct sockaddr *)&address,
               sizeof(address)) < 0) {
      fprintf(stderr, "tensor-va: cannot connect %s: %s\n", path,
              strerror(errno));
      if (context->socket_fd >= 0)
         close(context->socket_fd);
      context->socket_fd = -1;
      return false;
   }

   const char *component = getenv("TENSOR_MEDIACODEC_COMPONENT");
   if (!component || !*component)
      component = "c2.exynos.h264.decoder";
   unsigned fps = 60;
   const char *fps_text = getenv("TENSOR_MEDIACODEC_FPS");
   if (fps_text && *fps_text) {
      unsigned long parsed = strtoul(fps_text, NULL, 10);
      if (parsed > 0 && parsed <= UINT_MAX)
         fps = (unsigned)parsed;
   }
   if (!tensor_send_message(context->socket_fd, TMC_CONFIG, 0,
                            context->width, context->height, fps, component,
                            (uint32_t)strlen(component) + 1u) ||
       !tensor_process_until(driver, context, TMC_READY)) {
      close(context->socket_fd);
      context->socket_fd = -1;
      return false;
   }
   return true;
}

static VAStatus
tensor_terminate(VADriverContextP ctx)
{
   struct tensor_va_driver *driver = ctx->pDriverData;
   if (driver) {
      for (unsigned i = 0; i < TENSOR_MAX_CONTEXTS; i++) {
         if (driver->contexts[i].socket_fd >= 0 &&
             driver->contexts[i].used)
            close(driver->contexts[i].socket_fd);
         free(driver->contexts[i].slice_data);
      }
      for (unsigned i = 0; i < TENSOR_MAX_SURFACES; i++) {
         if (driver->surfaces[i].used)
            tensor_release_surface(&driver->surfaces[i]);
      }
      for (unsigned i = 0; i < TENSOR_MAX_BUFFERS; i++)
         if (driver->buffers[i].owns_data)
            free(driver->buffers[i].data);
      free(driver);
   }
   ctx->pDriverData = NULL;
   return VA_STATUS_SUCCESS;
}

static VAStatus
tensor_query_config_profiles(VADriverContextP ctx, VAProfile *profiles,
                             int *num_profiles)
{
   if (!profiles || !num_profiles)
      return VA_STATUS_ERROR_INVALID_PARAMETER;

   profiles[0] = VAProfileH264ConstrainedBaseline;
   profiles[1] = VAProfileH264Main;
   profiles[2] = VAProfileH264High;
   *num_profiles = 3;
   return VA_STATUS_SUCCESS;
}

static VAStatus
tensor_query_config_entrypoints(VADriverContextP ctx, VAProfile profile,
                                VAEntrypoint *entrypoints,
                                int *num_entrypoints)
{
   if (!entrypoints || !num_entrypoints)
      return VA_STATUS_ERROR_INVALID_PARAMETER;
   if (!tensor_profile_supported(profile))
      return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;

   entrypoints[0] = VAEntrypointVLD;
   *num_entrypoints = 1;
   return VA_STATUS_SUCCESS;
}

static VAStatus
tensor_get_config_attributes(VADriverContextP ctx, VAProfile profile,
                             VAEntrypoint entrypoint,
                             VAConfigAttrib *attributes, int num_attributes)
{
   if (!tensor_profile_supported(profile))
      return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
   if (entrypoint != VAEntrypointVLD)
      return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;

   for (int i = 0; i < num_attributes; i++) {
      if (attributes[i].type == VAConfigAttribRTFormat)
         attributes[i].value = VA_RT_FORMAT_YUV420;
      else
         attributes[i].value = VA_ATTRIB_NOT_SUPPORTED;
   }
   return VA_STATUS_SUCCESS;
}

static VAStatus
tensor_create_config(VADriverContextP ctx, VAProfile profile,
                     VAEntrypoint entrypoint, VAConfigAttrib *attributes,
                     int num_attributes, VAConfigID *config_id)
{
   struct tensor_va_driver *driver = ctx->pDriverData;

   if (!config_id)
      return VA_STATUS_ERROR_INVALID_PARAMETER;
   if (!tensor_profile_supported(profile))
      return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
   if (entrypoint != VAEntrypointVLD)
      return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;

   for (int i = 0; i < num_attributes; i++) {
      if (attributes[i].type == VAConfigAttribRTFormat &&
          !(attributes[i].value & VA_RT_FORMAT_YUV420))
         return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
   }

   for (unsigned i = 0; i < TENSOR_MAX_CONFIGS; i++) {
      if (driver->configs[i].used)
         continue;
      driver->configs[i].used = true;
      driver->configs[i].profile = profile;
      driver->configs[i].entrypoint = entrypoint;
      *config_id = i + 1;
      return VA_STATUS_SUCCESS;
   }
   return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
}

static VAStatus
tensor_destroy_config(VADriverContextP ctx, VAConfigID config_id)
{
   struct tensor_va_driver *driver = ctx->pDriverData;
   struct tensor_va_config *config = tensor_config(driver, config_id);
   if (!config)
      return VA_STATUS_ERROR_INVALID_CONFIG;
   memset(config, 0, sizeof(*config));
   return VA_STATUS_SUCCESS;
}

static VAStatus
tensor_query_config_attributes(VADriverContextP ctx, VAConfigID config_id,
                               VAProfile *profile, VAEntrypoint *entrypoint,
                               VAConfigAttrib *attributes,
                               int *num_attributes)
{
   struct tensor_va_driver *driver = ctx->pDriverData;
   struct tensor_va_config *config = tensor_config(driver, config_id);
   if (!config)
      return VA_STATUS_ERROR_INVALID_CONFIG;
   if (profile)
      *profile = config->profile;
   if (entrypoint)
      *entrypoint = config->entrypoint;
   if (attributes && num_attributes && *num_attributes > 0) {
      attributes[0].type = VAConfigAttribRTFormat;
      attributes[0].value = VA_RT_FORMAT_YUV420;
      *num_attributes = 1;
   } else if (num_attributes) {
      *num_attributes = 0;
   }
   return VA_STATUS_SUCCESS;
}

#define TENSOR_UNIMPLEMENTED(name, arguments) \
   static VAStatus name arguments { return VA_STATUS_ERROR_UNIMPLEMENTED; }

static VAStatus tensor_destroy_surfaces(VADriverContextP ctx,
                                        VASurfaceID *surfaces,
                                        int num_surfaces);

static VAStatus
tensor_create_surfaces(VADriverContextP ctx, int width, int height, int format,
                       int num_surfaces, VASurfaceID *surfaces)
{
   struct tensor_va_driver *driver = ctx->pDriverData;
   if (tensor_debug_enabled())
      fprintf(stderr,
              "tensor-va: create-surfaces %dx%d format=0x%x count=%d\n",
              width, height, format, num_surfaces);
   if (!surfaces || width <= 0 || height <= 0 || num_surfaces <= 0)
      return VA_STATUS_ERROR_INVALID_PARAMETER;
   if (!(format & VA_RT_FORMAT_YUV420))
      return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

   int created = 0;
   for (unsigned i = 0; i < TENSOR_MAX_SURFACES && created < num_surfaces;
        i++) {
      if (driver->surfaces[i].used)
         continue;
      memset(&driver->surfaces[i], 0, sizeof(driver->surfaces[i]));
      driver->surfaces[i].dma_fd = -1;
      if (!tensor_allocate_surface(&driver->surfaces[i], (unsigned)width,
                                   (unsigned)height))
         break;
      driver->surfaces[i].used = true;
      driver->surfaces[i].pts_us = -1;
      driver->surfaces[i].status = VASurfaceReady;
      surfaces[created++] = i + 1;
   }
   if (created == num_surfaces)
      return VA_STATUS_SUCCESS;

   tensor_destroy_surfaces(ctx, surfaces, created);
   return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
}

static VAStatus
tensor_destroy_surfaces(VADriverContextP ctx, VASurfaceID *surfaces,
                        int num_surfaces)
{
   struct tensor_va_driver *driver = ctx->pDriverData;
   if (!surfaces || num_surfaces < 0)
      return VA_STATUS_ERROR_INVALID_PARAMETER;
   for (int i = 0; i < num_surfaces; i++) {
      struct tensor_va_surface *surface = tensor_surface(driver, surfaces[i]);
      if (!surface)
         return VA_STATUS_ERROR_INVALID_SURFACE;
      tensor_release_surface(surface);
   }
   return VA_STATUS_SUCCESS;
}

static VAStatus
tensor_create_context(VADriverContextP ctx, VAConfigID config_id, int width,
                      int height, int flags, VASurfaceID *targets,
                      int num_targets, VAContextID *context_id)
{
   struct tensor_va_driver *driver = ctx->pDriverData;
   if (!context_id || width <= 0 || height <= 0 ||
       !tensor_config(driver, config_id))
      return VA_STATUS_ERROR_INVALID_PARAMETER;
   for (int i = 0; i < num_targets; i++) {
      if (!tensor_surface(driver, targets[i]))
         return VA_STATUS_ERROR_INVALID_SURFACE;
   }
   for (unsigned i = 0; i < TENSOR_MAX_CONTEXTS; i++) {
      if (driver->contexts[i].used)
         continue;
      struct tensor_va_context *context = &driver->contexts[i];
      memset(context, 0, sizeof(*context));
      context->used = true;
      context->config_id = config_id;
      context->width = (unsigned)width;
      context->height = (unsigned)height;
      context->current_target = VA_INVALID_ID;
      context->socket_fd = -1;
      if (!tensor_connect_service(driver, context)) {
         memset(context, 0, sizeof(*context));
         return VA_STATUS_ERROR_OPERATION_FAILED;
      }
      *context_id = i + 1;
      return VA_STATUS_SUCCESS;
   }
   return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
}

static VAStatus
tensor_destroy_context(VADriverContextP ctx, VAContextID context_id)
{
   struct tensor_va_driver *driver = ctx->pDriverData;
   struct tensor_va_context *context = tensor_context(driver, context_id);
   if (!context)
      return VA_STATUS_ERROR_INVALID_CONTEXT;
   if (context->socket_fd >= 0)
      close(context->socket_fd);
   free(context->slice_data);
   memset(context, 0, sizeof(*context));
   return VA_STATUS_SUCCESS;
}

static VAStatus
tensor_create_buffer(VADriverContextP ctx, VAContextID context_id,
                     VABufferType type, unsigned size, unsigned num_elements,
                     void *data, VABufferID *buffer_id)
{
   struct tensor_va_driver *driver = ctx->pDriverData;
   if (!buffer_id || !size || !num_elements ||
       !tensor_context(driver, context_id) || size > SIZE_MAX / num_elements)
      return VA_STATUS_ERROR_INVALID_PARAMETER;

   for (unsigned i = 0; i < TENSOR_MAX_BUFFERS; i++) {
      if (driver->buffers[i].used)
         continue;
      size_t bytes = (size_t)size * num_elements;
      void *copy = calloc(1, bytes);
      if (!copy)
         return VA_STATUS_ERROR_ALLOCATION_FAILED;
      if (data)
         memcpy(copy, data, bytes);
      driver->buffers[i].used = true;
      driver->buffers[i].type = type;
      driver->buffers[i].size = size;
      driver->buffers[i].num_elements = num_elements;
      driver->buffers[i].data = copy;
      driver->buffers[i].owns_data = true;
      *buffer_id = i + 1;
      return VA_STATUS_SUCCESS;
   }
   return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
}

static VAStatus
tensor_buffer_set_num_elements(VADriverContextP ctx, VABufferID buffer_id,
                               unsigned num_elements)
{
   struct tensor_va_driver *driver = ctx->pDriverData;
   struct tensor_va_buffer *buffer = tensor_buffer(driver, buffer_id);
   if (!buffer || !num_elements || num_elements > buffer->num_elements)
      return VA_STATUS_ERROR_INVALID_BUFFER;
   buffer->num_elements = num_elements;
   return VA_STATUS_SUCCESS;
}

static VAStatus
tensor_map_buffer(VADriverContextP ctx, VABufferID buffer_id, void **data)
{
   struct tensor_va_driver *driver = ctx->pDriverData;
   struct tensor_va_buffer *buffer = tensor_buffer(driver, buffer_id);
   if (!buffer || !data)
      return VA_STATUS_ERROR_INVALID_BUFFER;
   *data = buffer->data;
   return VA_STATUS_SUCCESS;
}

static VAStatus
tensor_unmap_buffer(VADriverContextP ctx, VABufferID buffer_id)
{
   struct tensor_va_driver *driver = ctx->pDriverData;
   return tensor_buffer(driver, buffer_id) ? VA_STATUS_SUCCESS
                                           : VA_STATUS_ERROR_INVALID_BUFFER;
}

static VAStatus
tensor_destroy_buffer(VADriverContextP ctx, VABufferID buffer_id)
{
   struct tensor_va_driver *driver = ctx->pDriverData;
   struct tensor_va_buffer *buffer = tensor_buffer(driver, buffer_id);
   if (!buffer)
      return VA_STATUS_ERROR_INVALID_BUFFER;
   if (buffer->owns_data)
      free(buffer->data);
   memset(buffer, 0, sizeof(*buffer));
   return VA_STATUS_SUCCESS;
}

static VAStatus
tensor_begin_picture(VADriverContextP ctx, VAContextID context_id,
                     VASurfaceID target_id)
{
   struct tensor_va_driver *driver = ctx->pDriverData;
   struct tensor_va_context *context = tensor_context(driver, context_id);
   struct tensor_va_surface *surface = tensor_surface(driver, target_id);
   if (!context)
      return VA_STATUS_ERROR_INVALID_CONTEXT;
   if (!surface)
      return VA_STATUS_ERROR_INVALID_SURFACE;
   context->current_target = target_id;
   context->slice_size = 0;
   context->have_picture = false;
   context->have_first_slice = false;
   context->pps_id = 0;
   surface->pts_us = -1;
   surface->status = VASurfaceRendering;
   return VA_STATUS_SUCCESS;
}

static VAStatus
tensor_render_picture(VADriverContextP ctx, VAContextID context_id,
                      VABufferID *buffer_ids, int num_buffers)
{
   struct tensor_va_driver *driver = ctx->pDriverData;
   struct tensor_va_context *context = tensor_context(driver, context_id);
   if (!context || context->current_target == VA_INVALID_ID)
      return VA_STATUS_ERROR_INVALID_CONTEXT;
   if (!buffer_ids || num_buffers < 0)
      return VA_STATUS_ERROR_INVALID_PARAMETER;

   for (int i = 0; i < num_buffers; i++) {
      struct tensor_va_buffer *buffer = tensor_buffer(driver, buffer_ids[i]);
      if (!buffer)
         return VA_STATUS_ERROR_INVALID_BUFFER;
      size_t length = (size_t)buffer->size * buffer->num_elements;
      if (buffer->type == VAPictureParameterBufferType && buffer->data &&
          length >= sizeof(context->picture)) {
         memcpy(&context->picture, buffer->data, sizeof(context->picture));
         context->have_picture = true;
      } else if (buffer->type == VASliceParameterBufferType && buffer->data &&
                 length >= sizeof(context->first_slice) &&
                 !context->have_first_slice) {
         memcpy(&context->first_slice, buffer->data,
                sizeof(context->first_slice));
         context->have_first_slice = true;
      } else if (buffer->type == VASliceDataBufferType && buffer->data) {
         const uint8_t *bytes = buffer->data;
         static const uint8_t start_code[] = {0, 0, 0, 1};
         if (!context->slice_size)
            context->pps_id = tensor_slice_pps_id(bytes, length);
         if (!tensor_append_bytes(&context->slice_data, &context->slice_size,
                                  &context->slice_capacity, start_code,
                                  sizeof(start_code)) ||
             !tensor_append_bytes(&context->slice_data, &context->slice_size,
                                  &context->slice_capacity, bytes, length))
            return VA_STATUS_ERROR_ALLOCATION_FAILED;
      }
   }
   return VA_STATUS_SUCCESS;
}

static VAStatus
tensor_end_picture(VADriverContextP ctx, VAContextID context_id)
{
   struct tensor_va_driver *driver = ctx->pDriverData;
   struct tensor_va_context *context = tensor_context(driver, context_id);
   if (!context || context->current_target == VA_INVALID_ID)
      return VA_STATUS_ERROR_INVALID_CONTEXT;
   struct tensor_va_surface *surface =
      tensor_surface(driver, context->current_target);
   if (!surface)
      return VA_STATUS_ERROR_INVALID_SURFACE;

   uint8_t *packet = NULL;
   size_t packet_size = 0;
   size_t packet_capacity = 0;
   if (!context->slice_size ||
       !tensor_append_parameter_sets(driver, context, &packet, &packet_size,
                                     &packet_capacity) ||
       !tensor_append_bytes(&packet, &packet_size, &packet_capacity,
                            context->slice_data, context->slice_size) ||
       packet_size > TMC_MAX_PAYLOAD) {
      free(packet);
      return VA_STATUS_ERROR_DECODING_ERROR;
   }

   int64_t pts_us = ((int64_t)context->submitted_frames + 1) * 1000;
   surface->pts_us = pts_us;
   bool sent = tensor_send_message(context->socket_fd, TMC_PACKET, pts_us, 0,
                                   0, 0, packet, (uint32_t)packet_size);
   free(packet);
   if (!sent || !tensor_process_until(driver, context, TMC_ACK))
      return VA_STATUS_ERROR_OPERATION_FAILED;

   context->current_target = VA_INVALID_ID;
   context->submitted_frames++;
   return VA_STATUS_SUCCESS;
}

static VAStatus
tensor_sync_surface(VADriverContextP ctx, VASurfaceID surface_id)
{
   struct tensor_va_driver *driver = ctx->pDriverData;
   struct tensor_va_surface *surface = tensor_surface(driver, surface_id);
   if (!surface)
      return VA_STATUS_ERROR_INVALID_SURFACE;
   if (surface->status == VASurfaceReady)
      return VA_STATUS_SUCCESS;

   for (unsigned i = 0; i < TENSOR_MAX_CONTEXTS; i++) {
      struct tensor_va_context *context = &driver->contexts[i];
      if (!context->used || context->socket_fd < 0)
         continue;
      for (unsigned attempt = 0; attempt < 5; attempt++) {
         if (!tensor_send_message(context->socket_fd, TMC_DRAIN,
                                  surface->pts_us, 0, 0, 0, NULL, 0) ||
             !tensor_process_until(driver, context, TMC_ACK))
            return VA_STATUS_ERROR_OPERATION_FAILED;
         if (surface->status == VASurfaceReady)
            return VA_STATUS_SUCCESS;
      }
   }
   return VA_STATUS_ERROR_TIMEDOUT;
}

static VAStatus
tensor_query_surface_status(VADriverContextP ctx, VASurfaceID surface_id,
                            VASurfaceStatus *status)
{
   struct tensor_va_driver *driver = ctx->pDriverData;
   struct tensor_va_surface *surface = tensor_surface(driver, surface_id);
   if (!surface || !status)
      return VA_STATUS_ERROR_INVALID_SURFACE;
   *status = surface->status;
   return VA_STATUS_SUCCESS;
}

static VAStatus
tensor_query_image_formats(VADriverContextP ctx, VAImageFormat *formats,
                           int *num_formats)
{
   if (!formats || !num_formats)
      return VA_STATUS_ERROR_INVALID_PARAMETER;
   memset(&formats[0], 0, sizeof(formats[0]));
   formats[0].fourcc = VA_FOURCC_NV12;
   *num_formats = 1;
   return VA_STATUS_SUCCESS;
}

static void
tensor_set_surface_attribute(VASurfaceAttrib *attribute,
                             VASurfaceAttribType type, uint32_t flags,
                             int value)
{
   memset(attribute, 0, sizeof(*attribute));
   attribute->type = type;
   attribute->flags = flags;
   attribute->value.type = VAGenericValueTypeInteger;
   attribute->value.value.i = value;
}

static VAStatus
tensor_query_surface_attributes(VADriverContextP ctx, VAConfigID config_id,
                                VASurfaceAttrib *attributes,
                                unsigned *num_attributes)
{
   struct tensor_va_driver *driver = ctx->pDriverData;
   const unsigned required = 6;
   if (!num_attributes || !tensor_config(driver, config_id))
      return VA_STATUS_ERROR_INVALID_PARAMETER;
   if (!attributes) {
      *num_attributes = required;
      return VA_STATUS_SUCCESS;
   }
   if (*num_attributes < required) {
      *num_attributes = required;
      return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
   }

   tensor_set_surface_attribute(&attributes[0], VASurfaceAttribPixelFormat,
                                VA_SURFACE_ATTRIB_GETTABLE |
                                   VA_SURFACE_ATTRIB_SETTABLE,
                                VA_FOURCC_NV12);
   tensor_set_surface_attribute(&attributes[1], VASurfaceAttribMinWidth,
                                VA_SURFACE_ATTRIB_GETTABLE, 16);
   tensor_set_surface_attribute(&attributes[2], VASurfaceAttribMaxWidth,
                                VA_SURFACE_ATTRIB_GETTABLE, 4096);
   tensor_set_surface_attribute(&attributes[3], VASurfaceAttribMinHeight,
                                VA_SURFACE_ATTRIB_GETTABLE, 16);
   tensor_set_surface_attribute(&attributes[4], VASurfaceAttribMaxHeight,
                                VA_SURFACE_ATTRIB_GETTABLE, 4096);
   tensor_set_surface_attribute(&attributes[5], VASurfaceAttribMemoryType,
                                VA_SURFACE_ATTRIB_GETTABLE |
                                   VA_SURFACE_ATTRIB_SETTABLE,
                                VA_SURFACE_ATTRIB_MEM_TYPE_VA);
   *num_attributes = required;
   return VA_STATUS_SUCCESS;
}

static VAStatus
tensor_create_surfaces2(VADriverContextP ctx, unsigned format, unsigned width,
                        unsigned height, VASurfaceID *surfaces,
                        unsigned num_surfaces, VASurfaceAttrib *attributes,
                        unsigned num_attributes)
{
   if (tensor_debug_enabled())
      fprintf(stderr,
              "tensor-va: create-surfaces2 %ux%u format=0x%x count=%u "
              "attrs=%u\n",
              width, height, format, num_surfaces, num_attributes);
   for (unsigned i = 0; i < num_attributes; i++) {
      if (tensor_debug_enabled()) {
         fprintf(stderr, "tensor-va: surface-attr type=%d value-type=%d",
                 attributes[i].type, attributes[i].value.type);
         if (attributes[i].value.type == VAGenericValueTypeInteger ||
             attributes[i].value.type == 0)
            fprintf(stderr, " value=0x%x", attributes[i].value.value.i);
         fputc('\n', stderr);
      }
      if (attributes[i].type == VASurfaceAttribPixelFormat &&
          ((attributes[i].value.type != VAGenericValueTypeInteger &&
            attributes[i].value.type != 0) ||
           attributes[i].value.value.i != (int)VA_FOURCC_NV12))
         return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
      if (attributes[i].type == VASurfaceAttribMemoryType &&
          ((attributes[i].value.type != VAGenericValueTypeInteger &&
            attributes[i].value.type != 0) ||
           !(attributes[i].value.value.i & VA_SURFACE_ATTRIB_MEM_TYPE_VA)))
         return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
   }
   return tensor_create_surfaces(ctx, (int)width, (int)height, (int)format,
                                 (int)num_surfaces, surfaces);
}
static VAStatus
tensor_create_image(VADriverContextP ctx, VAImageFormat *format, int width,
                    int height, VAImage *image)
{
   struct tensor_va_driver *driver = ctx->pDriverData;
   if (!format || format->fourcc != VA_FOURCC_NV12 || width <= 0 ||
       height <= 0 || !image)
      return VA_STATUS_ERROR_INVALID_PARAMETER;
   if ((size_t)width > SIZE_MAX / (size_t)height)
      return VA_STATUS_ERROR_ALLOCATION_FAILED;
   size_t pixels = (size_t)width * (size_t)height;
   size_t bytes = pixels + pixels / 2;

   unsigned image_index = TENSOR_MAX_IMAGES;
   unsigned buffer_index = TENSOR_MAX_BUFFERS;
   for (unsigned i = 0; i < TENSOR_MAX_IMAGES; i++) {
      if (!driver->images[i].used) {
         image_index = i;
         break;
      }
   }
   for (unsigned i = 0; i < TENSOR_MAX_BUFFERS; i++) {
      if (!driver->buffers[i].used) {
         buffer_index = i;
         break;
      }
   }
   if (image_index == TENSOR_MAX_IMAGES || buffer_index == TENSOR_MAX_BUFFERS)
      return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;

   void *data = malloc(bytes);
   if (!data)
      return VA_STATUS_ERROR_ALLOCATION_FAILED;
   struct tensor_va_buffer *buffer = &driver->buffers[buffer_index];
   buffer->used = true;
   buffer->type = VAImageBufferType;
   buffer->size = bytes > UINT_MAX ? UINT_MAX : (unsigned)bytes;
   buffer->num_elements = 1;
   buffer->data = data;
   buffer->owns_data = true;

   struct tensor_va_image *record = &driver->images[image_index];
   memset(record, 0, sizeof(*record));
   record->used = true;
   record->surface_id = VA_INVALID_ID;
   record->image.image_id = image_index + 1;
   record->image.format = *format;
   record->image.buf = buffer_index + 1;
   record->image.width = (unsigned)width;
   record->image.height = (unsigned)height;
   record->image.data_size = buffer->size;
   record->image.num_planes = 2;
   record->image.pitches[0] = (unsigned)width;
   record->image.pitches[1] = (unsigned)width;
   record->image.offsets[0] = 0;
   record->image.offsets[1] = (unsigned)pixels;
   *image = record->image;
   return VA_STATUS_SUCCESS;
}

static VAStatus
tensor_derive_image(VADriverContextP ctx, VASurfaceID surface_id,
                    VAImage *image)
{
   struct tensor_va_driver *driver = ctx->pDriverData;
   struct tensor_va_surface *surface = tensor_surface(driver, surface_id);
   if (!surface || !image)
      return VA_STATUS_ERROR_INVALID_SURFACE;
   VAStatus sync_status = tensor_sync_surface(ctx, surface_id);
   if (sync_status != VA_STATUS_SUCCESS)
      return sync_status;
   if (tensor_debug_enabled())
      fprintf(stderr, "tensor-va: derive-image surface=%u\n", surface_id);

   unsigned image_index = TENSOR_MAX_IMAGES;
   unsigned buffer_index = TENSOR_MAX_BUFFERS;
   for (unsigned i = 0; i < TENSOR_MAX_IMAGES; i++) {
      if (!driver->images[i].used) {
         image_index = i;
         break;
      }
   }
   for (unsigned i = 0; i < TENSOR_MAX_BUFFERS; i++) {
      if (!driver->buffers[i].used) {
         buffer_index = i;
         break;
      }
   }
   if (image_index == TENSOR_MAX_IMAGES || buffer_index == TENSOR_MAX_BUFFERS)
      return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;

   size_t image_size =
      (size_t)surface->stride * surface->slice_height * 3u / 2u;
   if (image_size > surface->data_size || image_size > UINT_MAX)
      return VA_STATUS_ERROR_ALLOCATION_FAILED;

   struct tensor_va_buffer *buffer = &driver->buffers[buffer_index];
   buffer->used = true;
   buffer->type = VAImageBufferType;
   buffer->size = (unsigned)image_size;
   buffer->num_elements = 1;
   buffer->data = surface->data;
   buffer->owns_data = false;

   struct tensor_va_image *record = &driver->images[image_index];
   memset(record, 0, sizeof(*record));
   record->used = true;
   record->surface_id = surface_id;
   record->image.image_id = image_index + 1;
   record->image.format.fourcc = VA_FOURCC_NV12;
   record->image.buf = buffer_index + 1;
   record->image.width = surface->width;
   record->image.height = surface->height;
   record->image.data_size = buffer->size;
   record->image.num_planes = 2;
   record->image.pitches[0] = surface->stride;
   record->image.pitches[1] = surface->stride;
   record->image.offsets[0] = 0;
   record->image.offsets[1] = surface->stride * surface->slice_height;
   *image = record->image;
   return VA_STATUS_SUCCESS;
}

static VAStatus
tensor_destroy_image(VADriverContextP ctx, VAImageID image_id)
{
   struct tensor_va_driver *driver = ctx->pDriverData;
   if (!image_id || image_id > TENSOR_MAX_IMAGES ||
       !driver->images[image_id - 1].used)
      return VA_STATUS_ERROR_INVALID_IMAGE;
   struct tensor_va_image *record = &driver->images[image_id - 1];
   struct tensor_va_buffer *buffer =
      tensor_buffer(driver, record->image.buf);
   if (buffer) {
      if (buffer->owns_data)
         free(buffer->data);
      memset(buffer, 0, sizeof(*buffer));
   }
   memset(record, 0, sizeof(*record));
   return VA_STATUS_SUCCESS;
}
TENSOR_UNIMPLEMENTED(tensor_set_image_palette,
                     (VADriverContextP ctx, VAImageID image,
                      unsigned char *palette))
static VAStatus
tensor_get_image(VADriverContextP ctx, VASurfaceID surface_id, int x, int y,
                 unsigned width, unsigned height, VAImageID image_id)
{
   struct tensor_va_driver *driver = ctx->pDriverData;
   struct tensor_va_surface *surface = tensor_surface(driver, surface_id);
   if (!surface || !image_id || image_id > TENSOR_MAX_IMAGES ||
       !driver->images[image_id - 1].used)
      return VA_STATUS_ERROR_INVALID_IMAGE;
   struct tensor_va_image *record = &driver->images[image_id - 1];
   struct tensor_va_buffer *buffer =
      tensor_buffer(driver, record->image.buf);
   if (!buffer || !buffer->data || x < 0 || y < 0 ||
       (unsigned)x + width > surface->width ||
       (unsigned)y + height > surface->height ||
       width > record->image.width || height > record->image.height ||
       (x & 1) || (y & 1) || (width & 1) || (height & 1))
      return VA_STATUS_ERROR_INVALID_PARAMETER;
   VAStatus sync_status = tensor_sync_surface(ctx, surface_id);
   if (sync_status != VA_STATUS_SUCCESS)
      return sync_status;
   if (tensor_debug_enabled())
      fprintf(stderr,
              "tensor-va: get-image surface=%u region=%d,%d %ux%u\n",
              surface_id, x, y, width, height);

   uint8_t *destination = buffer->data;
   for (unsigned row = 0; row < height; row++) {
      memcpy(destination + record->image.offsets[0] +
                row * record->image.pitches[0],
             surface->data + ((unsigned)y + row) * surface->stride +
                (unsigned)x,
             width);
   }
   const uint8_t *source_chroma =
      surface->data + surface->stride * surface->slice_height;
   for (unsigned row = 0; row < height / 2; row++) {
      memcpy(destination + record->image.offsets[1] +
                row * record->image.pitches[1],
             source_chroma + ((unsigned)y / 2 + row) * surface->stride +
                (unsigned)x,
             width);
   }
   return VA_STATUS_SUCCESS;
}
TENSOR_UNIMPLEMENTED(tensor_put_image,
                     (VADriverContextP ctx, VASurfaceID surface,
                      VAImageID image, int src_x, int src_y,
                      unsigned src_width, unsigned src_height, int dst_x,
                      int dst_y, unsigned dst_width, unsigned dst_height))
TENSOR_UNIMPLEMENTED(tensor_query_subpicture_formats,
                     (VADriverContextP ctx, VAImageFormat *formats,
                      unsigned *flags, unsigned *num_formats))
TENSOR_UNIMPLEMENTED(tensor_create_subpicture,
                     (VADriverContextP ctx, VAImageID image,
                      VASubpictureID *subpicture))
TENSOR_UNIMPLEMENTED(tensor_destroy_subpicture,
                     (VADriverContextP ctx, VASubpictureID subpicture))
TENSOR_UNIMPLEMENTED(tensor_set_subpicture_image,
                     (VADriverContextP ctx, VASubpictureID subpicture,
                      VAImageID image))
TENSOR_UNIMPLEMENTED(tensor_set_subpicture_chromakey,
                     (VADriverContextP ctx, VASubpictureID subpicture,
                      unsigned minimum, unsigned maximum, unsigned mask))
TENSOR_UNIMPLEMENTED(tensor_set_subpicture_alpha,
                     (VADriverContextP ctx, VASubpictureID subpicture,
                      float alpha))
TENSOR_UNIMPLEMENTED(tensor_associate_subpicture,
                     (VADriverContextP ctx, VASubpictureID subpicture,
                      VASurfaceID *surfaces, int num_surfaces, short src_x,
                      short src_y, unsigned short src_width,
                      unsigned short src_height, short dst_x, short dst_y,
                      unsigned short dst_width, unsigned short dst_height,
                      unsigned flags))
TENSOR_UNIMPLEMENTED(tensor_deassociate_subpicture,
                     (VADriverContextP ctx, VASubpictureID subpicture,
                      VASurfaceID *surfaces, int num_surfaces))
TENSOR_UNIMPLEMENTED(tensor_query_display_attributes,
                     (VADriverContextP ctx, VADisplayAttribute *attributes,
                      int *num_attributes))
TENSOR_UNIMPLEMENTED(tensor_get_display_attributes,
                     (VADriverContextP ctx, VADisplayAttribute *attributes,
                      int num_attributes))
TENSOR_UNIMPLEMENTED(tensor_set_display_attributes,
                     (VADriverContextP ctx, VADisplayAttribute *attributes,
                      int num_attributes))

static VAStatus
tensor_export_surface_handle(VADriverContextP ctx, VASurfaceID surface_id,
                             uint32_t mem_type, uint32_t flags,
                             void *descriptor)
{
   struct tensor_va_driver *driver = ctx->pDriverData;
   struct tensor_va_surface *surface = tensor_surface(driver, surface_id);
   if (!surface || !descriptor)
      return VA_STATUS_ERROR_INVALID_SURFACE;
   if (mem_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2)
      return VA_STATUS_ERROR_UNSUPPORTED_MEMORY_TYPE;
   VAStatus sync_status = tensor_sync_surface(ctx, surface_id);
   if (sync_status != VA_STATUS_SUCCESS)
      return sync_status;
   if (tensor_debug_enabled())
      fprintf(stderr,
              "tensor-va: export surface=%u mem=0x%x flags=0x%x fd=%d\n",
              surface_id, mem_type, flags, surface->dma_fd);

   VADRMPRIMESurfaceDescriptor *prime = descriptor;
   memset(prime, 0, sizeof(*prime));
   prime->fourcc = VA_FOURCC_NV12;
   prime->width = surface->width;
   prime->height = surface->height;
   prime->num_objects = 1;
   prime->objects[0].fd = dup(surface->dma_fd);
   if (prime->objects[0].fd < 0)
      return VA_STATUS_ERROR_OPERATION_FAILED;
   prime->objects[0].size =
      surface->data_size > UINT_MAX ? UINT_MAX : (uint32_t)surface->data_size;
   prime->objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;

   uint32_t chroma_offset = surface->stride * surface->slice_height;
   if (flags & VA_EXPORT_SURFACE_SEPARATE_LAYERS) {
      prime->num_layers = 2;
      prime->layers[0].drm_format = DRM_FORMAT_R8;
      prime->layers[0].num_planes = 1;
      prime->layers[0].object_index[0] = 0;
      prime->layers[0].offset[0] = 0;
      prime->layers[0].pitch[0] = surface->stride;
      prime->layers[1].drm_format = DRM_FORMAT_GR88;
      prime->layers[1].num_planes = 1;
      prime->layers[1].object_index[0] = 0;
      prime->layers[1].offset[0] = chroma_offset;
      prime->layers[1].pitch[0] = surface->stride;
   } else {
      prime->num_layers = 1;
      prime->layers[0].drm_format = DRM_FORMAT_NV12;
      prime->layers[0].num_planes = 2;
      prime->layers[0].object_index[0] = 0;
      prime->layers[0].object_index[1] = 0;
      prime->layers[0].offset[0] = 0;
      prime->layers[0].offset[1] = chroma_offset;
      prime->layers[0].pitch[0] = surface->stride;
      prime->layers[0].pitch[1] = surface->stride;
   }
   return VA_STATUS_SUCCESS;
}

__attribute__((visibility("default"))) VAStatus
__vaDriverInit_1_14(VADriverContextP ctx)
{
   struct tensor_va_driver *driver;

   if (!ctx || !ctx->vtable)
      return VA_STATUS_ERROR_INVALID_CONTEXT;

   driver = calloc(1, sizeof(*driver));
   if (!driver)
      return VA_STATUS_ERROR_ALLOCATION_FAILED;

   ctx->pDriverData = driver;
   ctx->version_major = 0;
   ctx->version_minor = 1;
   ctx->max_profiles = 3;
   ctx->max_entrypoints = 1;
   ctx->max_attributes = 1;
   ctx->max_image_formats = 1;
   ctx->max_subpic_formats = 1;
   ctx->max_display_attributes = 1;
   ctx->str_vendor = "Tensor G1 rootless MediaCodec VA-API bridge";

   ctx->vtable->vaTerminate = tensor_terminate;
   ctx->vtable->vaQueryConfigProfiles = tensor_query_config_profiles;
   ctx->vtable->vaQueryConfigEntrypoints = tensor_query_config_entrypoints;
   ctx->vtable->vaGetConfigAttributes = tensor_get_config_attributes;
   ctx->vtable->vaCreateConfig = tensor_create_config;
   ctx->vtable->vaDestroyConfig = tensor_destroy_config;
   ctx->vtable->vaQueryConfigAttributes = tensor_query_config_attributes;
   ctx->vtable->vaCreateSurfaces = tensor_create_surfaces;
   ctx->vtable->vaDestroySurfaces = tensor_destroy_surfaces;
   ctx->vtable->vaCreateContext = tensor_create_context;
   ctx->vtable->vaDestroyContext = tensor_destroy_context;
   ctx->vtable->vaCreateBuffer = tensor_create_buffer;
   ctx->vtable->vaBufferSetNumElements = tensor_buffer_set_num_elements;
   ctx->vtable->vaMapBuffer = tensor_map_buffer;
   ctx->vtable->vaUnmapBuffer = tensor_unmap_buffer;
   ctx->vtable->vaDestroyBuffer = tensor_destroy_buffer;
   ctx->vtable->vaBeginPicture = tensor_begin_picture;
   ctx->vtable->vaRenderPicture = tensor_render_picture;
   ctx->vtable->vaEndPicture = tensor_end_picture;
   ctx->vtable->vaSyncSurface = tensor_sync_surface;
   ctx->vtable->vaQuerySurfaceStatus = tensor_query_surface_status;
   ctx->vtable->vaQueryImageFormats = tensor_query_image_formats;
   ctx->vtable->vaCreateImage = tensor_create_image;
   ctx->vtable->vaDeriveImage = tensor_derive_image;
   ctx->vtable->vaDestroyImage = tensor_destroy_image;
   ctx->vtable->vaSetImagePalette = tensor_set_image_palette;
   ctx->vtable->vaGetImage = tensor_get_image;
   ctx->vtable->vaPutImage = tensor_put_image;
   ctx->vtable->vaQuerySubpictureFormats = tensor_query_subpicture_formats;
   ctx->vtable->vaCreateSubpicture = tensor_create_subpicture;
   ctx->vtable->vaDestroySubpicture = tensor_destroy_subpicture;
   ctx->vtable->vaSetSubpictureImage = tensor_set_subpicture_image;
   ctx->vtable->vaSetSubpictureChromakey = tensor_set_subpicture_chromakey;
   ctx->vtable->vaSetSubpictureGlobalAlpha = tensor_set_subpicture_alpha;
   ctx->vtable->vaAssociateSubpicture = tensor_associate_subpicture;
   ctx->vtable->vaDeassociateSubpicture = tensor_deassociate_subpicture;
   ctx->vtable->vaQueryDisplayAttributes = tensor_query_display_attributes;
   ctx->vtable->vaGetDisplayAttributes = tensor_get_display_attributes;
   ctx->vtable->vaSetDisplayAttributes = tensor_set_display_attributes;
   ctx->vtable->vaCreateSurfaces2 = tensor_create_surfaces2;
   ctx->vtable->vaQuerySurfaceAttributes =
      tensor_query_surface_attributes;
   ctx->vtable->vaExportSurfaceHandle = tensor_export_surface_handle;

   return VA_STATUS_SUCCESS;
}
