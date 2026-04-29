
#ifndef INCLUDE_DEVICE_CONFIG_H_
#define INCLUDE_DEVICE_CONFIG_H_

#include "device_cfg_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "backend.h"
//PPKY
//#define PPKY_CONFIG_SIZE 0x100000 // 1мб &!&!&!&!&!!&

#define ZONE_NAME_SIZE	64 // 64 символа на имя зоны
#define ZONE_NUMBER		100 // количество зон

#define NUM_DEV_IN_MCU 32

// Заголовок области конфигурации во Flash
#define PPKY_CFG_HEADER_MAGIC 0x50504B59u /* 'P','P','K','Y' */

typedef struct {
	uint32_t magic;   // сигнатура
	uint16_t version; // версия формата
	uint32_t size;    // размер полезной части (PPKYCfg) в байтах
} PPKYConfigHeader;


typedef struct MKUCfg {
	UniqId	UId;

	uint32_t VDtype[NUM_DEV_IN_MCU];  /* 4 байта, выравнивание */
	uint32_t zone_delay;
	uint32_t module_delay[NUM_DEV_IN_MCU];
	VDeviceCfg	Devices[NUM_DEV_IN_MCU];

	uint8_t	reserv[64];
	/* резерв: sizeof(MKUCfg) кратно 4 */

} MKUCfg;

typedef struct PPKYCfg {

	UniqId	UId;

	uint8_t beep;
	uint8_t fire_mode; // режим тушения 0 - автоматический, 1 - автономный, 2 - ручной
	uint8_t _pad[2];  /* явное выравнивание под CfgDevices (offset 36), заменяет reserv  */

	MKUCfg	CfgDevices[32];

	int8_t zone_name[ZONE_NUMBER][ZONE_NAME_SIZE];
	uint8_t fire_and[ZONE_NUMBER]; // режим или\и (0 - или, 1 - И)

} PPKYCfg;
//END PPKY

typedef struct DeviceDPTConfig {
    /* Режим виртуального устройства:
     * 0 - ДПТ (пожар)
     * 1 - концевик (открытие)
     * 2 - кнопка (нажатие)
     * ВАЖНО: для VDeviceDPT это legacy-поле, на поведение ДПТ не влияет.
     */
    uint8_t mode;

    /* Использовать ли MAX31855 для определения пожара при КЗ:
     * 0 - игнорировать MAX, работать только по сопротивлению
     * 1 - использовать MAX
     */
    uint8_t use_max;

    /* Порог температуры MAX (°C) для "пожар".
     * Целое, только положительные значения. По умолчанию 60 °C.
     */
    uint16_t max_fire_threshold_c;

    /* Время стабилизации уровня (мс) перед сменой состояния линии.
     * По умолчанию 100 мс.
     */
    uint16_t state_change_delay_ms;

    /* резерв до полного размера VDeviceCfg::reserv (64 байта) */
    uint8_t reserved[VDEVICE_CFG_SIZE - 6];
} DeviceDPTConfig;

/* Вид виртуальной кнопки */
typedef enum DeviceButtonKind {
    DeviceButtonKind_StartSP   = 0, /* имитация нажатия ПУСК СП на ППКУ (через callback из app) */
    DeviceButtonKind_StartAll  = 1, /* широковещательный StartExtinguishment, data[0]=1 */
    DeviceButtonKind_StartZone = 2  /* запуск зон из массива zones[7] */
} DeviceButtonKind;

typedef struct DeviceButtonConfig {
    /* Совместимость с DeviceDPTConfig (общая часть для классификации линии) */
	/* пока не вижу смысла в этом
    uint8_t mode;
    uint8_t use_max;
    uint16_t max_fire_threshold_c;
    uint16_t state_change_delay_ms;
    */

    /* Параметры виртуальной кнопки */
    uint8_t button_kind;             /* DeviceButtonKind */
    uint8_t zones[7];                /* список зон для режима StartZone */

    /* резерв до полного размера VDeviceCfg::reserv (64 байта) */
    uint8_t reserved[VDEVICE_CFG_SIZE - (1 + 7)];
} DeviceButtonConfig;


typedef struct DeviceIgniterConfig {
	/* 0 - проверка КЗ включена (по умолчанию)
	 * 1 - проверка КЗ отключена, КЗ считается "Норма"
	 */
	uint8_t disable_sc_check;

	/* Пороги ADC (мВ): 0=ошибка, 1..break_low=норма, break_low..break_high=обрыв/КЗ, >break_high=ошибка.
	 * По умолчанию 1000, 3000 */
	uint16_t threshold_break_low;   /* мВ, нижняя граница "обрыв/КЗ" */
	uint16_t threshold_break_high;  /* мВ, выше — ошибка */

	/* Количество повторных циклов прожига при отсутствии обрыва (0 или 1). По умолчанию 1 */
	uint8_t burn_retry_count;

	/* резерв до полного размера VDeviceCfg::reserv (64 байта).
	 * Для текущего порядка полей есть 1 байт выравнивания перед threshold_break_low,
	 * поэтому используем 7 байт служебной части. */
	uint8_t reserved[VDEVICE_CFG_SIZE - 7];
} DeviceIgniterConfig;

typedef struct DeviceRelayConfig {
	/* Начальное состояние реле после Init:
	 * 0 - выключено, 1 - включено */
	uint8_t initial_state;

	/* Режим "сохранение состояния":
	 * 0 - отключено (по умолчанию),
	 * 1 - после каждого переключения записываем текущее состояние в initial_state
	 *     и сохраняем конфигурацию. */
	uint8_t persist_state_enabled;

	/* Инверсия обратной связи:
	 * 0 - feedback 1 означает "включено"
	 * 1 - feedback 0 означает "включено" */
	uint8_t feedback_inverted;

	/* Задержка перед переключением реле, секунды (0..255).
	 * По умолчанию 0 (без задержки). */
	uint8_t switch_delay_s;

	/* Время ожидания установления обратной связи после переключения, мс */
	uint16_t settle_time_ms;

	/* резерв до полного размера VDeviceCfg::reserv (64 байта) */
	uint8_t reserved[VDEVICE_CFG_SIZE - 6];
} DeviceRelayConfig;

typedef enum DeviceLimitSwitchFunction {
	DeviceLimitSwitchFunction_SetFault = 1,   /* выставить статус "неисправность" */
	DeviceLimitSwitchFunction_SetManual = 2,  /* перевести ППКУ в ручной режим */
	DeviceLimitSwitchFunction_SetAuto = 3,    /* перевести ППКУ в автоматический режим */
	DeviceLimitSwitchFunction_PauseStart = 4  /* пауза пуска (до отпускания) */
} DeviceLimitSwitchFunction;

typedef struct DeviceLimitSwitchConfig {
	/* Совместимая "голова" c DeviceDPTConfig */
	uint8_t mode;
	uint8_t use_max;
	uint16_t max_fire_threshold_c;
	uint16_t state_change_delay_ms;

	/* Новые параметры концевика */
	uint8_t trigger_delay_s;   /* задержка срабатывания, сек */
	uint8_t function;          /* DeviceLimitSwitchFunction */
	uint8_t normal_closed;     /* 0=NO, 1=NC */

	/* резерв до полного размера VDeviceCfg::reserv (64 байта) */
	uint8_t reserved[VDEVICE_CFG_SIZE - 9];
} DeviceLimitSwitchConfig;

#ifdef __cplusplus
static_assert(sizeof(DeviceDPTConfig) == VDEVICE_CFG_SIZE, "DeviceDPTConfig size mismatch");
static_assert(sizeof(DeviceButtonConfig) == VDEVICE_CFG_SIZE, "DeviceButtonConfig size mismatch");
static_assert(sizeof(DeviceIgniterConfig) == VDEVICE_CFG_SIZE, "DeviceIgniterConfig size mismatch");
static_assert(sizeof(DeviceRelayConfig) == VDEVICE_CFG_SIZE, "DeviceRelayConfig size mismatch");
static_assert(sizeof(DeviceLimitSwitchConfig) == VDEVICE_CFG_SIZE, "DeviceLimitSwitchConfig size mismatch");
#endif



#ifdef __cplusplus
}
#endif

#endif /* INCLUDE_DEVICE_CONFIG_H_ */
