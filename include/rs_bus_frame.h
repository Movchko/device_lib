#ifndef RS_BUS_FRAME_H
#define RS_BUS_FRAME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RS_BUS_PREAMBLE_0        0xA5u
#define RS_BUS_PREAMBLE_1        0x5Au
#define RS_BUS_BROADCAST_ADDR    0x00u
#define RS_BUS_ADDR_RESERVED     0xFFu
#define RS_BUS_MAX_PAYLOAD       512u
#define RS_BUS_FRAME_OVERHEAD    8u
#define RS_BUS_MAX_FRAME_SIZE    (2u + 1u + RS_BUS_FRAME_OVERHEAD + RS_BUS_MAX_PAYLOAD)

typedef enum {
    RS_BUS_FLAG_DIR     = 0x01u,
    RS_BUS_FLAG_MORE    = 0x02u,
    RS_BUS_FLAG_FRAG    = 0x04u,
    RS_BUS_FLAG_ACK_REQ = 0x80u
} RsBusFrameFlags;

typedef struct {
    uint8_t addr;
    uint8_t seq;
    uint8_t flags;
    uint8_t cmd;
    uint16_t payload_len;
    const uint8_t *payload;
} RsBusFrameView;

typedef struct {
    uint8_t frag_id;
    uint8_t frag_idx;
    uint8_t frag_total;
} RsBusFragHeader;

uint16_t RsBus_Checksum16(const uint8_t *data, uint16_t len);
uint16_t RsBus_FrameEncode(uint8_t *dst,
                           uint16_t dst_size,
                           uint8_t addr,
                           uint8_t seq,
                           uint8_t flags,
                           uint8_t cmd,
                           const uint8_t *payload,
                           uint16_t payload_len);
uint8_t RsBus_FrameDecode(const uint8_t *src,
                          uint16_t src_size,
                          RsBusFrameView *out_frame,
                          uint16_t *out_consumed);

#ifdef __cplusplus
}
#endif

#endif /* RS_BUS_FRAME_H */
