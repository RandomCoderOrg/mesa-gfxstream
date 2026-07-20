#ifndef TENSOR_MEDIACODEC_PERF_METRICS_H
#define TENSOR_MEDIACODEC_PERF_METRICS_H

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum tmc_perf_stage_id {
   TMC_PERF_RECEIVE_HEADER,
   TMC_PERF_RECEIVE_PAYLOAD,
   TMC_PERF_SURFACE_REGISTER,
   TMC_PERF_RELEASE_FENCE_ARM,
   TMC_PERF_CODEC_CONFIGURE_START,
   TMC_PERF_INPUT_DEQUEUE,
   TMC_PERF_INPUT_COPY,
   TMC_PERF_INPUT_QUEUE,
   TMC_PERF_OUTPUT_DEQUEUE,
   TMC_PERF_DMABUF_SYNC_START,
   TMC_PERF_SURFACE_CLEAR,
   TMC_PERF_SURFACE_COPY_Y,
   TMC_PERF_SURFACE_COPY_UV,
   TMC_PERF_DMABUF_SYNC_END,
   TMC_PERF_RELEASE_FENCE_SIGNAL,
   TMC_PERF_FRAME_SEND,
   TMC_PERF_ACK_SEND,
   TMC_PERF_CODEC_STOP,
   TMC_PERF_STAGE_COUNT,
};

static const char *const tmc_perf_stage_names[TMC_PERF_STAGE_COUNT] = {
   [TMC_PERF_RECEIVE_HEADER] = "receive_header",
   [TMC_PERF_RECEIVE_PAYLOAD] = "receive_payload",
   [TMC_PERF_SURFACE_REGISTER] = "surface_register",
   [TMC_PERF_RELEASE_FENCE_ARM] = "release_fence_arm",
   [TMC_PERF_CODEC_CONFIGURE_START] = "codec_configure_start",
   [TMC_PERF_INPUT_DEQUEUE] = "input_dequeue",
   [TMC_PERF_INPUT_COPY] = "input_copy",
   [TMC_PERF_INPUT_QUEUE] = "input_queue",
   [TMC_PERF_OUTPUT_DEQUEUE] = "output_dequeue",
   [TMC_PERF_DMABUF_SYNC_START] = "dmabuf_sync_start",
   [TMC_PERF_SURFACE_CLEAR] = "surface_clear",
   [TMC_PERF_SURFACE_COPY_Y] = "surface_copy_y",
   [TMC_PERF_SURFACE_COPY_UV] = "surface_copy_uv",
   [TMC_PERF_DMABUF_SYNC_END] = "dmabuf_sync_end",
   [TMC_PERF_RELEASE_FENCE_SIGNAL] = "release_fence_signal",
   [TMC_PERF_FRAME_SEND] = "frame_send",
   [TMC_PERF_ACK_SEND] = "ack_send",
   [TMC_PERF_CODEC_STOP] = "codec_stop",
};

struct tmc_perf_stat {
   uint64_t count;
   uint64_t total_ns;
   uint64_t min_ns;
   uint64_t max_ns;
   uint64_t bytes;
};

struct tmc_perf {
   FILE *output;
   bool owns_output;
   uint64_t started_ns;
   char run_id[64];
   struct tmc_perf_stat stages[TMC_PERF_STAGE_COUNT];
   uint64_t input_packets;
   uint64_t input_bytes;
   uint64_t output_frames;
   uint64_t output_bytes;
   uint64_t shared_frames;
   uint64_t surface_registrations;
   uint64_t output_empty_polls;
   uint64_t pts_remaps;
};

static inline uint64_t
tmc_perf_now_ns(void)
{
   struct timespec now;
   if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
      return 0;
   return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
          (uint64_t)now.tv_nsec;
}

static inline void
tmc_perf_init(struct tmc_perf *perf)
{
   memset(perf, 0, sizeof(*perf));
   const char *path = getenv("TENSOR_PERF_OUTPUT");
   if (!path || !path[0])
      return;
   if (strcmp(path, "-") == 0) {
      perf->output = stderr;
   } else {
      perf->output = fopen(path, "a");
      perf->owns_output = perf->output != NULL;
   }
   if (!perf->output)
      return;
   setvbuf(perf->output, NULL, _IOLBF, 0);
   perf->started_ns = tmc_perf_now_ns();
   snprintf(perf->run_id, sizeof(perf->run_id), "%ld-%" PRIu64,
            (long)getpid(), perf->started_ns);
}

static inline uint64_t
tmc_perf_begin(const struct tmc_perf *perf)
{
   return perf->output ? tmc_perf_now_ns() : 0;
}

static inline void
tmc_perf_record(struct tmc_perf *perf, enum tmc_perf_stage_id stage,
                uint64_t started_ns, uint64_t bytes)
{
   if (!perf->output || !started_ns || stage >= TMC_PERF_STAGE_COUNT)
      return;
   uint64_t elapsed = tmc_perf_now_ns() - started_ns;
   struct tmc_perf_stat *stat = &perf->stages[stage];
   if (!stat->count || elapsed < stat->min_ns)
      stat->min_ns = elapsed;
   if (elapsed > stat->max_ns)
      stat->max_ns = elapsed;
   stat->count++;
   stat->total_ns += elapsed;
   stat->bytes += bytes;
}

static inline void
tmc_perf_finish(struct tmc_perf *perf, bool complete)
{
   if (!perf->output)
      return;
   uint64_t elapsed = tmc_perf_now_ns() - perf->started_ns;
   fprintf(perf->output,
           "{\"schema\":\"tensor-perf-v1\",\"kind\":\"session\","
           "\"run_id\":\"%s\",\"process\":\"mediacodec-service\","
           "\"complete\":%s,\"elapsed_ns\":%" PRIu64 ","
           "\"input_packets\":%" PRIu64 ",\"input_bytes\":%" PRIu64 ","
           "\"output_frames\":%" PRIu64 ",\"output_bytes\":%" PRIu64 ","
           "\"shared_frames\":%" PRIu64 ","
           "\"surface_registrations\":%" PRIu64 ","
           "\"output_empty_polls\":%" PRIu64 ",\"pts_remaps\":%" PRIu64 "}\n",
           perf->run_id, complete ? "true" : "false", elapsed,
           perf->input_packets, perf->input_bytes, perf->output_frames,
           perf->output_bytes, perf->shared_frames,
           perf->surface_registrations, perf->output_empty_polls,
           perf->pts_remaps);
   for (unsigned i = 0; i < TMC_PERF_STAGE_COUNT; i++) {
      const struct tmc_perf_stat *stat = &perf->stages[i];
      if (!stat->count)
         continue;
      fprintf(perf->output,
              "{\"schema\":\"tensor-perf-v1\",\"kind\":\"stage\","
              "\"run_id\":\"%s\",\"process\":\"mediacodec-service\","
              "\"stage\":\"%s\",\"count\":%" PRIu64 ","
              "\"total_ns\":%" PRIu64 ",\"min_ns\":%" PRIu64 ","
              "\"max_ns\":%" PRIu64 ",\"bytes\":%" PRIu64 "}\n",
              perf->run_id, tmc_perf_stage_names[i], stat->count,
              stat->total_ns, stat->min_ns, stat->max_ns, stat->bytes);
   }
   if (perf->owns_output)
      fclose(perf->output);
   perf->output = NULL;
}

#endif
