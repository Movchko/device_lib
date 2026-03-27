#ifndef INCLUDE_DEVICE_CFG_COMMON_H_
#define INCLUDE_DEVICE_CFG_COMMON_H_

#include <stdint.h>

/* Размер буфера конфигурации одного виртуального устройства. */
#define VDEVICE_CFG_SIZE 64u


#define DEVICE_PPKY_TYPE 10u
#define DEVICE_IGNITER_TYPE 11u
#define DEVICE_DPT_TYPE 12u

#define DEVICE_MCU_IGN_TYPE 13u
#define DEVICE_MCU_TC_TYPE 14u

#define DEVICE_BUTTON_TYPE 15u
#define DEVICE_LSWITCH_TYPE 16u
#define DEVICE_RELAY_TYPE 17u

#define DEVICE_MCU_K1 20u
#define DEVICE_MCU_K2 21u
#define DEVICE_MCU_K3 22u
#define DEVICE_MCU_KR 23u

/* Общая “сырая” область под данные конкретных устройств.
 * В C++ разные типы конфигов читаются через reinterpret_cast из reserv.
 */
typedef struct VDeviceCfg {
	uint8_t reserv[VDEVICE_CFG_SIZE];
} VDeviceCfg;

#endif /* INCLUDE_DEVICE_CFG_COMMON_H_ */

