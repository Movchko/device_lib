#ifndef INCLUDE_ESP_PROTOCOL_H_
#define INCLUDE_ESP_PROTOCOL_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BSU_PKT_TYPE_ESP_ACTIVITY  2u
#define BSU_PKT_TYPE_ESP_CMD       3u
#define BSU_PKT_TYPE_ESP_CAN       4u
#define BSU_PKT_TYPE_ESP_UART      5u

#define ESP_UART_BODY_MAX          246u
#define ESP_ACTIVITY_PAYLOAD_SIZE  20u
#define ESP_CONFIG_PAYLOAD_SIZE    12u

enum EspCmd {
	ESP_CMD_WIFI_ENABLE  = 1,
	ESP_CMD_WIFI_DISABLE = 2,
	ESP_CMD_SET_CONFIG   = 3,
	ESP_CMD_PING         = 4,
};

typedef struct __attribute__((packed)) EspActivityPayload {
	uint8_t  wifi_enabled;
	uint8_t  tcp_connected;
	uint8_t  wifi_clients;
	uint8_t  flags;
	uint32_t uptime_sec;
	uint8_t  ex_can_on;
	uint8_t  ex_can_protocol;
	uint8_t  reserved[2];
	uint32_t ex_can_baudrate;
	uint32_t ex_rs485_baudrate;
} EspActivityPayload;

typedef struct __attribute__((packed)) EspConfigPayload {
	uint8_t  ex_can_on;
	uint8_t  ex_can_protocol;
	uint8_t  wifi_block;
	uint8_t  reserved;
	uint32_t ex_can_baudrate;
	uint32_t ex_rs485_baudrate;
} EspConfigPayload;

#define ESP_ACTIVITY_FLAG_CONFIG_APPLIED  (1u << 0)

#ifdef __cplusplus
}
#endif

#endif /* INCLUDE_ESP_PROTOCOL_H_ */
