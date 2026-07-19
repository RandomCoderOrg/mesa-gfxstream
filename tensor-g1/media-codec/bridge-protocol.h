#ifndef TENSOR_MEDIACODEC_BRIDGE_PROTOCOL_H
#define TENSOR_MEDIACODEC_BRIDGE_PROTOCOL_H

#include <stdint.h>

#define TMC_MAGIC 0x31434d54u /* "TMC1" in little endian */
#define TMC_VERSION 1u
#define TMC_MAX_PAYLOAD (16u * 1024u * 1024u)

#define TMC_CAP_SHARED_SURFACE (1u << 0)
#define TMC_FRAME_FLAG_SHARED_SURFACE (1u << 31)

enum tmc_message_type {
   TMC_CONFIG = 1,
   TMC_PACKET = 2,
   TMC_INPUT_EOS = 3,
   /* Drain currently available output without ending the decoder session. */
   TMC_DRAIN = 4,
   /* Register a DMA-BUF destination for the frame identified by pts_us.
    * arg0/arg1/arg2 carry stride, slice height, and mapped byte size. */
   TMC_SURFACE = 5,
   TMC_READY = 101,
   TMC_FORMAT = 102,
   TMC_FRAME = 103,
   TMC_OUTPUT_EOS = 104,
   TMC_ACK = 105,
   TMC_ERROR = 106,
};

struct tmc_message {
   uint32_t magic;
   uint16_t version;
   uint16_t type;
   uint32_t payload_size;
   uint32_t flags;
   int64_t pts_us;
   uint32_t arg0;
   uint32_t arg1;
   uint32_t arg2;
   uint32_t arg3;
};

_Static_assert(sizeof(struct tmc_message) == 40,
               "bridge protocol header must remain stable");

#endif
