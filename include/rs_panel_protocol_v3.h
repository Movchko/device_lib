/**

 * rs_panel_protocol_v3.h — контракт panel↔ППКУ v3 (эталон поведения ППКУ1).

 *

 * POLL/RSP payload начинается с ver=0x03. Старые payload без ver несовместимы.

 * CMD коды POLL/RSP те же (0x01 / 0x81); семантика payload — v3.

 */

#ifndef RS_PANEL_PROTOCOL_V3_H

#define RS_PANEL_PROTOCOL_V3_H



#include <stdint.h>

#include "rs_panel_protocol.h"



#ifdef __cplusplus

extern "C" {

#endif



#define RS_PANEL_V3_VERSION                 0x03u



#define RS_PANEL_V3_MAX_ZONES               16u

#define RS_PANEL_V3_MAX_FAULTS              32u

#define RS_PANEL_V3_ZONE_NAME_LEN           24u

#define RS_PANEL_V3_FAULT_TEXT_LEN          48u /* legacy text FAULT_OP — deprecated */

#define RS_PANEL_V3_EVENT_REPLY_MAX         200u

#define RS_PANEL_V3_MAX_CATALOG_MCU         32u

#define RS_PANEL_V3_MAX_MCU_CHANNELS        4u

#define RS_PANEL_V3_MAX_ZONE_CATALOG        100u



#define RS_PANEL_V3_SYS_READY_DELAY_MS      20000u

#define RS_PANEL_V3_TIME_SYNC_PERIOD_MS     10000u



#define RS_PANEL_V3_SYS_READY               (1u << 0)

#define RS_PANEL_V3_SYS_FIRE_ACTIVE         (1u << 1)

#define RS_PANEL_V3_SYS_CONFIG_ACTIVE       (1u << 2)

#define RS_PANEL_V3_SYS_HAS_FAULTS          (1u << 3)

#define RS_PANEL_V3_SYS_TIME_VALID          (1u << 4)

#define RS_PANEL_V3_SYS_POWER_INPUT_FAULT   (1u << 5)



#define RS_PANEL_V3_RSP_FLAG_NEED_CATALOG   (1u << 0)



#define RS_PANEL_V3_FAULT_FLAG_ATTENTION    (1u << 0)

#define RS_PANEL_V3_FAULT_FLAG_HAS_MCU      (1u << 1)

#define RS_PANEL_V3_FAULT_FLAG_HAS_CHANNEL  (1u << 2)

#define RS_PANEL_V3_FAULT_FLAG_HAS_CAN      (1u << 3)

#define RS_PANEL_V3_FAULT_FLAG_HAS_EXTRA    (1u << 4)



typedef enum {

    RS_PANEL_V3_ACK_NONE = 0u,

    RS_PANEL_V3_ACK_OK = 1u,

    RS_PANEL_V3_ACK_DENIED = 2u,

    RS_PANEL_V3_ACK_IGNORED = 3u

} RsPanelV3AckResult;



typedef enum {

    RS_PANEL_V3_ZONE_IDLE = 0u,

    RS_PANEL_V3_ZONE_COUNTDOWN = 1u,

    RS_PANEL_V3_ZONE_EXTINGUISHING = 2u,

    RS_PANEL_V3_ZONE_EXT_DONE = 3u,

    RS_PANEL_V3_ZONE_FIRE_STOPPED = 4u,

    RS_PANEL_V3_ZONE_PAUSED = 5u,

    RS_PANEL_V3_ZONE_FIRE1 = 6u,

    RS_PANEL_V3_ZONE_EXT_FAIL = 7u,

    RS_PANEL_V3_ZONE_LAUNCH_BLOCKED = 8u,

    RS_PANEL_V3_ZONE_EXT_ABORT = 9u

} RsPanelV3ZoneStatus;



/* Коды неисправностей (карта v3). */

typedef enum {

    RS_PANEL_V3_FAULT_NONE = 0u,

    RS_PANEL_V3_FAULT_LINE_BREAK = 1u,

    RS_PANEL_V3_FAULT_LINE_SHORT = 2u,

    RS_PANEL_V3_FAULT_LINE_OTHER = 3u,

    RS_PANEL_V3_FAULT_MCU_CAN_BREAK = 4u,

    RS_PANEL_V3_FAULT_MCU_CAN_SHORT = 5u,

    RS_PANEL_V3_FAULT_PPKU_CAN_BREAK = 6u,

    RS_PANEL_V3_FAULT_PPKU_CAN_SHORT = 7u,

    RS_PANEL_V3_FAULT_MCU_POSITION = 8u,

    RS_PANEL_V3_FAULT_DEVICE_MISSING = 9u,

    RS_PANEL_V3_FAULT_DEVICE_FOUND = 10u,

    RS_PANEL_V3_FAULT_CONFIG_MISMATCH = 11u,

    RS_PANEL_V3_FAULT_PANEL_JOURNAL = 12u,

    RS_PANEL_V3_FAULT_PPKU_POWER_IN1 = 13u,

    RS_PANEL_V3_FAULT_PPKU_POWER_IN2 = 14u,

    RS_PANEL_V3_FAULT_PPKU_POWER_OUT1 = 15u,

    RS_PANEL_V3_FAULT_PPKU_POWER_OUT2 = 16u,

    RS_PANEL_V3_FAULT_ATTN_LSWITCH = 17u,

    RS_PANEL_V3_FAULT_ATTN_DPT = 18u

} RsPanelV3FaultCode;



typedef enum {

    RS_PANEL_V3_STREAM_OP_NONE = 0u,

    RS_PANEL_V3_STREAM_OP_CLEAR = 1u,

    RS_PANEL_V3_STREAM_OP_ADD = 2u

} RsPanelV3StreamOp;



typedef enum {

    RS_PANEL_V3_FAULT_EVT_NONE = 0u,

    RS_PANEL_V3_FAULT_EVT_CLEAR_ALL = 1u,

    RS_PANEL_V3_FAULT_EVT_SET = 2u,

    RS_PANEL_V3_FAULT_EVT_CLEAR = 3u

} RsPanelV3FaultEvtOp;



/* legacy text fault — deprecated */

typedef enum {

    RS_PANEL_V3_FAULT_OP_NONE = 0u,

    RS_PANEL_V3_FAULT_OP_CLEAR = 1u,

    RS_PANEL_V3_FAULT_OP_ADD = 2u,

    RS_PANEL_V3_FAULT_OP_REMOVE = 3u

} RsPanelV3FaultOp;



typedef enum {

    RS_PANEL_V3_EVT_NONE = 0u,

    RS_PANEL_V3_EVT_START_ALL_COMMIT = 1u,

    RS_PANEL_V3_EVT_START_SP = 2u,

    RS_PANEL_V3_EVT_STOP_LAUNCH = 3u,

    RS_PANEL_V3_EVT_FIRE_RESET = 4u,

    RS_PANEL_V3_EVT_SOUND_SET = 5u,

    RS_PANEL_V3_EVT_WIFI_SET = 6u,

    RS_PANEL_V3_EVT_JOURNAL_COUNT = 7u,

    RS_PANEL_V3_EVT_JOURNAL_GET = 8u,

    RS_PANEL_V3_EVT_JOURNAL_GET_N = 9u,

    RS_PANEL_V3_EVT_ZONE_BLOCK_LIST = 10u,

    RS_PANEL_V3_EVT_ZONE_BLOCK_SET = 11u,

    RS_PANEL_V3_EVT_DEVICES_COUNT = 12u,

    RS_PANEL_V3_EVT_DEVICES_GET = 13u,

    RS_PANEL_V3_EVT_DEVICES_GET_N = 14u,

    RS_PANEL_V3_EVT_EXT_CAN_SET = 15u

} RsPanelV3EventType;



typedef enum {

    RS_PANEL_V3_TAG_ACK = 0x01u,

    RS_PANEL_V3_TAG_SYS = 0x02u,

    RS_PANEL_V3_TAG_TIME = 0x03u,

    RS_PANEL_V3_TAG_ZONES = 0x04u,

    RS_PANEL_V3_TAG_FAULT_OP = 0x05u,      /* deprecated */

    RS_PANEL_V3_TAG_CONFIG = 0x06u,

    RS_PANEL_V3_TAG_EVENT_REPLY = 0x07u,

    RS_PANEL_V3_TAG_ZONE_NAMES = 0x08u,

    RS_PANEL_V3_TAG_DEVICES = 0x09u,

    RS_PANEL_V3_TAG_FAULT_EVT = 0x0Au

} RsPanelV3PollTag;



typedef enum {

    RS_PANEL_V3_CFG_IDLE = 0u,

    RS_PANEL_V3_CFG_RUNNING = 1u,

    RS_PANEL_V3_CFG_SUCCESS = 2u,

    RS_PANEL_V3_CFG_FAIL = 3u

} RsPanelV3ConfigPhase;



typedef struct {

    uint8_t seq;

    uint8_t result;

} RsPanelV3Ack;



typedef struct {

    uint16_t flags;

} RsPanelV3Sys;



typedef struct {

    uint8_t hour;

    uint8_t min;

    uint8_t sec;

    uint8_t day;

    uint8_t month;

    uint8_t year;

} RsPanelV3Time;



typedef struct {

    uint8_t zone;

    uint8_t status;

    uint8_t remaining_s;

    char name[RS_PANEL_V3_ZONE_NAME_LEN];

} RsPanelV3ZoneItem;



typedef struct {

    uint8_t count;

    RsPanelV3ZoneItem items[RS_PANEL_V3_MAX_ZONES];

} RsPanelV3Zones;



typedef struct {

    uint8_t op;

    char text[RS_PANEL_V3_FAULT_TEXT_LEN];

} RsPanelV3FaultOpItem;



typedef struct {

    uint8_t phase;

    uint8_t percent;

} RsPanelV3Config;



typedef struct {

    uint8_t seq;

    uint8_t type;

    uint8_t result;

    uint16_t payload_len;

    uint8_t payload[RS_PANEL_V3_EVENT_REPLY_MAX];

} RsPanelV3EventReply;



typedef struct {

    uint8_t op; /* RsPanelV3StreamOp */

    uint8_t zone;

    char name[RS_PANEL_V3_ZONE_NAME_LEN];

} RsPanelV3ZoneNameItem;



typedef struct {

    uint8_t ch_type;

    uint8_t ch_l_adr;

} RsPanelV3McuChannelItem;



typedef struct {

    uint8_t op; /* RsPanelV3StreamOp */

    uint8_t d_type;

    uint8_t h_adr;

    uint8_t l_adr;

    uint8_t zone;

    uint32_t uid0;

    uint32_t uid1;

    uint32_t uid2;

    uint8_t n_ch;

    RsPanelV3McuChannelItem ch[RS_PANEL_V3_MAX_MCU_CHANNELS];

} RsPanelV3DeviceItem;



typedef struct {

    uint8_t h_adr;

    uint8_t l_adr;

    uint32_t uid0;

    uint32_t uid1;

    uint32_t uid2;

} RsPanelV3McuKey;



typedef struct {

    uint8_t op; /* RsPanelV3FaultEvtOp */

    uint8_t code; /* RsPanelV3FaultCode */

    uint8_t flags;

    RsPanelV3McuKey mcu;

    uint8_t ch_type;

    uint8_t ch_l_adr;

    uint8_t can_idx;

    int16_t extra;

    uint8_t panel_addr;

} RsPanelV3FaultEvtItem;



typedef struct {

    RsPanelV3Ack ack;

    RsPanelV3Sys sys;

    uint8_t has_time;

    RsPanelV3Time time;

    uint8_t has_zones;

    RsPanelV3Zones zones;

    uint8_t has_fault_op;

    RsPanelV3FaultOpItem fault_op;

    uint8_t has_zone_names;

    RsPanelV3ZoneNameItem zone_names;

    uint8_t has_devices;

    RsPanelV3DeviceItem device;

    uint8_t has_fault_evt;

    RsPanelV3FaultEvtItem fault_evt;

    uint8_t has_config;

    RsPanelV3Config config;

    uint8_t has_event_reply;

    RsPanelV3EventReply event_reply;

} RsPanelV3Poll;



typedef struct {

    uint8_t seq;

    uint8_t type;

    uint8_t zone;

    uint8_t u8_a;

    uint16_t u16_a;

    uint16_t u16_b;

} RsPanelV3Event;



typedef struct {

    RsPanelV3Event event;

    uint16_t devices_crc;

    uint16_t faults_crc;

    uint8_t flags;

} RsPanelV3Rsp;



uint16_t RsPanelV3_FaultListCrc(const char (*lines)[RS_PANEL_V3_FAULT_TEXT_LEN], uint16_t count);



uint16_t RsPanelV3_Crc16Ccitt(const uint8_t *data, uint16_t len);

uint16_t RsPanelV3_FaultEvtCrc(const RsPanelV3FaultEvtItem *items, uint16_t count);

uint16_t RsPanelV3_DeviceCatalogCrc(const RsPanelV3DeviceItem *items, uint16_t count);

uint16_t RsPanelV3_ZoneCatalogCrc(const uint8_t (*names)[RS_PANEL_V3_ZONE_NAME_LEN],

                                  const uint8_t *zone_ids,

                                  uint16_t count);

/* Инкрементальные CRC (без полного кэша каталога в RAM). */

uint16_t RsPanelV3_DeviceCatalogCrc_Add(uint16_t crc, const RsPanelV3DeviceItem *item);

uint16_t RsPanelV3_ZoneCatalogCrc_Add(uint16_t crc, uint8_t zone_id, const char *name);



uint8_t RsPanelV3_FaultEvtEqual(const RsPanelV3FaultEvtItem *a, const RsPanelV3FaultEvtItem *b);

uint16_t RsPanelV3_FaultEvtEncodeBody(uint8_t *dst, uint16_t dst_size,

                                      const RsPanelV3FaultEvtItem *item);

uint8_t RsPanelV3_FaultEvtDecodeBody(const uint8_t *src, uint16_t len,

                                     RsPanelV3FaultEvtItem *out);



uint16_t RsPanelV3_EncodePoll(uint8_t *dst, uint16_t dst_size, const RsPanelV3Poll *poll);

uint8_t RsPanelV3_DecodePoll(const uint8_t *src, uint16_t src_len, RsPanelV3Poll *out);



uint16_t RsPanelV3_EncodeRsp(uint8_t *dst, uint16_t dst_size, const RsPanelV3Rsp *rsp);

uint8_t RsPanelV3_DecodeRsp(const uint8_t *src, uint16_t src_len, RsPanelV3Rsp *out);



#ifdef __cplusplus

}

#endif



#endif /* RS_PANEL_PROTOCOL_V3_H */

