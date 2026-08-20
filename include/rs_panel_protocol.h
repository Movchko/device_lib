#ifndef RS_PANEL_PROTOCOL_H
#define RS_PANEL_PROTOCOL_H

#include <stdint.h>
#include "rs_bus_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RS_PANEL_MAX_PANELS          8u
#define RS_PANEL_MAX_CAPS_BUTTONS    16u
#define RS_PANEL_MAX_CAPS_LEDS       16u
#define RS_PANEL_MAX_POLL_BTN_EVENTS 8u
#define RS_PANEL_MAX_POLL_UI_EVENTS  8u
#define RS_PANEL_MAX_LED_ITEMS       16u

typedef enum {
    RS_PANEL_ROLE_NORMAL = 0,
    RS_PANEL_ROLE_PRIMARY = 1
} RsPanelRole;

typedef enum {
    RS_PANEL_CMD_POLL        = 0x01u,
    RS_PANEL_CMD_LED         = 0x20u,
    RS_PANEL_CMD_SOUND       = 0x21u,
    RS_PANEL_CMD_UI_NAV      = 0x30u,
    RS_PANEL_CMD_UI_DATA     = 0x31u,
    RS_PANEL_CMD_TIME        = 0x32u,
    RS_PANEL_CMD_CAPS_REQ    = 0xF0u,
    RS_PANEL_CMD_PROFILE_SET = 0xF1u,
    RS_PANEL_CMD_PANEL_RESET = 0xF2u,
    RS_PANEL_RSP_POLL        = 0x81u,
    RS_PANEL_RSP_CAPS        = 0xF0u,
    RS_PANEL_RSP_ACK         = 0xFEu
} RsPanelCommand;

typedef enum {
    RS_PANEL_BTN_ESC = 0x01u,
    RS_PANEL_BTN_UP = 0x02u,
    RS_PANEL_BTN_DOWN = 0x03u,
    RS_PANEL_BTN_ENTER = 0x04u,
    RS_PANEL_BTN_STOP = 0x05u,
    RS_PANEL_BTN_START_SP = 0x06u,
    RS_PANEL_BTN_START_ALL = 0x07u
} RsBtnType;

typedef enum {
    RS_PANEL_BUTTON_RESET = 0u,
    RS_PANEL_BUTTON_PRESS = 1u,
    RS_PANEL_BUTTON_LONG_PRESS = 2u,
    RS_PANEL_BUTTON_ERROR = 3u
} RsButtonState;

typedef enum {
    RS_PANEL_LED_POWER = 0x10u,
    RS_PANEL_LED_NORM = 0x11u,
    RS_PANEL_LED_START = 0x12u,
    RS_PANEL_LED_STOP = 0x13u,
    RS_PANEL_LED_ERR = 0x14u,
    RS_PANEL_LED_FIRE = 0x15u,
    RS_PANEL_LED_AUTO_OFF = 0x16u,
    RS_PANEL_LED_BUT_START_ALL = 0x20u,
    RS_PANEL_LED_BUT_STOP = 0x21u,
    RS_PANEL_LED_BUT_START_SP = 0x22u,
    RS_PANEL_LED_BUT_ENTER = 0x23u,
    RS_PANEL_LED_BUT_ESC = 0x24u,
    RS_PANEL_LED_LBL_START_ALL = 0x30u,
    RS_PANEL_LED_LBL_STOP = 0x31u,
    RS_PANEL_LED_LBL_START_SP = 0x32u
} RsLedType;

typedef enum {
    RS_PANEL_LED_MODE_OFF = 0u,
    RS_PANEL_LED_MODE_ON = 1u,
    RS_PANEL_LED_MODE_BLINK = 2u,
    RS_PANEL_LED_MODE_BRIGHT = 3u
} RsLedMode;

typedef enum {
    RS_PANEL_SOUND_OFF = 0u,
    RS_PANEL_SOUND_FAULT = 1u,
    RS_PANEL_SOUND_ATTN = 2u,
    RS_PANEL_SOUND_FIRE = 3u,
    RS_PANEL_SOUND_FIRE1 = 4u,
    RS_PANEL_SOUND_START = 5u,
    RS_PANEL_SOUND_START_ALL_HOLD = 6u,
    RS_PANEL_SOUND_BTN_ACK = 7u,
    RS_PANEL_SOUND_CUSTOM = 8u
} RsSoundProfile;

typedef enum {
    RS_PANEL_UI_ACTION_OPEN = 0u,
    RS_PANEL_UI_ACTION_CLOSE = 1u,
    RS_PANEL_UI_ACTION_BACK = 2u,
    RS_PANEL_UI_ACTION_REPLACE = 3u
} RsUiNavAction;

typedef enum {
    RS_PANEL_SCREEN_LOGO = 0x0000u,
    RS_PANEL_SCREEN_MAIN = 0x0001u,
    RS_PANEL_SCREEN_MENU_ROOT = 0x0010u,
    RS_PANEL_SCREEN_MENU_SETTINGS = 0x0011u,
    RS_PANEL_SCREEN_MENU_DEVICES = 0x0012u,
    RS_PANEL_SCREEN_MENU_DEVICE_DETAIL = 0x0013u,
    RS_PANEL_SCREEN_MENU_CONFIG = 0x0014u,
    RS_PANEL_SCREEN_MENU_JOURNAL = 0x0015u,
    RS_PANEL_SCREEN_MENU_JOURNAL_DETAIL = 0x0016u,
    RS_PANEL_SCREEN_MENU_CONNECTION = 0x0017u,
    RS_PANEL_SCREEN_MENU_SOUND = 0x0018u,
    RS_PANEL_SCREEN_MENU_BLOCK_ZONE = 0x0019u,
    RS_PANEL_SCREEN_BLANK = 0x00FFu
} RsUiScreenId;

typedef enum {
    RS_PANEL_UI_DATA_MAIN_FIRE = 0x01u,
    RS_PANEL_UI_DATA_MAIN_WARN = 0x02u,
    RS_PANEL_UI_DATA_DATETIME = 0x03u,
    RS_PANEL_UI_DATA_MENU_LIST = 0x10u,
    RS_PANEL_UI_DATA_MENU_VALUE = 0x11u,
    RS_PANEL_UI_DATA_MENU_TOGGLE = 0x12u,
    RS_PANEL_UI_DATA_MENU_SELF_TEST = 0x13u,
    RS_PANEL_UI_DATA_JOURNAL_LIST = 0x20u,
    RS_PANEL_UI_DATA_JOURNAL_DETAIL = 0x21u,
    RS_PANEL_UI_DATA_DEVICE_LIST = 0x22u,
    RS_PANEL_UI_DATA_DEVICE_DETAIL = 0x23u,
    RS_PANEL_UI_DATA_ZONE_MODE_LIST = 0x24u,
    RS_PANEL_UI_DATA_CONNECTION_STATUS = 0x30u,
    RS_PANEL_UI_DATA_CONFIG_STATUS = 0x31u
} RsUiDataSubId;

typedef enum {
    RS_PANEL_UI_EVT_NAV = 0x01u,
    RS_PANEL_UI_EVT_CONFIRM = 0x02u,
    RS_PANEL_UI_EVT_BACK = 0x03u,
    RS_PANEL_UI_EVT_MENU_SELECT = 0x04u,
    RS_PANEL_UI_EVT_JOURNAL_OPEN = 0x05u
} RsUiEventType;

typedef enum {
    RS_PANEL_PROFILE_SET_ORIENTATION = 0x01u,
    RS_PANEL_PROFILE_SET_BTN_MASK = 0x02u,
    RS_PANEL_PROFILE_SET_LED_MASK = 0x03u,
    RS_PANEL_PROFILE_SET_JOURNAL_LINES = 0x04u,
    RS_PANEL_PROFILE_SET_FACTORY_RESET = 0x0Fu
} RsProfileSetSub;

typedef struct {
    uint8_t flags;
    uint8_t ack_seq;
} RsPanelPollReq;

typedef struct {
    uint8_t type;
    uint8_t state;
    uint8_t level;
} RsPanelButtonEvent;

typedef struct {
    uint8_t evt_type;
    uint16_t p1;
    uint16_t p2;
} RsPanelUiEvent;

typedef struct {
    uint8_t status;
    uint8_t evt_count;
    RsPanelButtonEvent btn_events[RS_PANEL_MAX_POLL_BTN_EVENTS];
    uint8_t ui_evt_count;
    RsPanelUiEvent ui_events[RS_PANEL_MAX_POLL_UI_EVENTS];
} RsPanelPollRsp;

typedef struct {
    uint16_t fw_ver;
    uint16_t hw_id;
    uint8_t ui_profile;
    uint8_t orientation;
    uint16_t disp_w;
    uint16_t disp_h;
    uint8_t journal_lines;
    uint8_t btn_count;
    uint8_t btn_list[RS_PANEL_MAX_CAPS_BUTTONS];
    uint8_t led_count;
    uint8_t led_list[RS_PANEL_MAX_CAPS_LEDS];
    uint8_t flags;
    uint8_t status;
} RsPanelCaps;

typedef struct {
    uint8_t type;
    uint8_t mode;
    uint8_t value;
} RsPanelLedItem;

typedef struct {
    uint8_t count;
    RsPanelLedItem items[RS_PANEL_MAX_LED_ITEMS];
} RsPanelLedCmd;

typedef struct {
    uint8_t profile;
    uint8_t mute;
    uint16_t on_ms;
    uint16_t off_ms;
    uint8_t pulses;
    uint16_t repeat_ms;
} RsPanelSoundCmd;

typedef struct {
    uint8_t hour;
    uint8_t min;
    uint8_t sec;
    uint8_t day;
    uint8_t month;
    uint8_t year;
} RsPanelTimeCmd;

typedef struct {
    uint16_t screen_id;
    uint8_t action;
    uint16_t param;
} RsPanelUiNavCmd;

typedef struct {
    uint8_t sub_id;
    const uint8_t *payload;
    uint16_t payload_len;
} RsPanelUiDataCmd;

typedef struct {
    uint8_t sub;
    union {
        uint8_t orientation;
        uint8_t btn_enable;
        uint16_t led_enable;
        uint8_t journal_lines;
    } value;
} RsPanelProfileSetCmd;

uint16_t RsPanel_EncodePollReq(uint8_t *dst, uint16_t dst_size, const RsPanelPollReq *req);
uint8_t RsPanel_DecodePollReq(const uint8_t *src, uint16_t src_len, RsPanelPollReq *out_req);
uint16_t RsPanel_EncodePollRsp(uint8_t *dst, uint16_t dst_size, const RsPanelPollRsp *rsp);
uint8_t RsPanel_DecodePollRsp(const uint8_t *src, uint16_t src_len, RsPanelPollRsp *out_rsp);
uint16_t RsPanel_EncodeCaps(uint8_t *dst, uint16_t dst_size, const RsPanelCaps *caps);
uint8_t RsPanel_DecodeCaps(const uint8_t *src, uint16_t src_len, RsPanelCaps *out_caps);
uint16_t RsPanel_EncodeLedCmd(uint8_t *dst, uint16_t dst_size, const RsPanelLedCmd *cmd);
uint8_t RsPanel_DecodeLedCmd(const uint8_t *src, uint16_t src_len, RsPanelLedCmd *out_cmd);
uint16_t RsPanel_EncodeSoundCmd(uint8_t *dst, uint16_t dst_size, const RsPanelSoundCmd *cmd);
uint8_t RsPanel_DecodeSoundCmd(const uint8_t *src, uint16_t src_len, RsPanelSoundCmd *out_cmd);
uint16_t RsPanel_EncodeTimeCmd(uint8_t *dst, uint16_t dst_size, const RsPanelTimeCmd *cmd);
uint8_t RsPanel_DecodeTimeCmd(const uint8_t *src, uint16_t src_len, RsPanelTimeCmd *out_cmd);
uint16_t RsPanel_EncodeUiNavCmd(uint8_t *dst, uint16_t dst_size, const RsPanelUiNavCmd *cmd);
uint8_t RsPanel_DecodeUiNavCmd(const uint8_t *src, uint16_t src_len, RsPanelUiNavCmd *out_cmd);
uint8_t RsPanel_DecodeUiDataCmd(const uint8_t *src, uint16_t src_len, RsPanelUiDataCmd *out_cmd);
uint8_t RsPanel_DecodeProfileSetCmd(const uint8_t *src, uint16_t src_len, RsPanelProfileSetCmd *out_cmd);

#ifdef __cplusplus
}
#endif

#endif /* RS_PANEL_PROTOCOL_H */
