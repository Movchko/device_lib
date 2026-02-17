
#ifndef INCLUDE_DEVICE_CONFIG_H_
#define INCLUDE_DEVICE_CONFIG_H_

#include "device.hpp"
#include "backend.h"
//PPKY
//#define PPKY_CONFIG_SIZE 0x100000 // 1мб &!&!&!&!&!!&

#define ZONE_NAME_SIZE	64 // 64 символа на имя зоны
#define ZONE_NUMBER		100 // количество зон

// Заголовок области конфигурации во Flash
#define PPKY_CFG_HEADER_MAGIC 0x50504B59u /* 'P','P','K','Y' */

typedef struct {
	uint32_t magic;   // сигнатура
	uint16_t version; // версия формата
	uint16_t size;    // размер полезной части (PPKYCfg) в байтах
} PPKYConfigHeader;


struct MKUCfg {

	UniqId	UId;
	VDeviceCfg	Devices[16];

    /* . резерв нужен чтобы бесшовно обновлять устройство с имзенением структуры,
     * при этом резерв уменьшать на кол-во давленных новых данных
     */
    uint8_t reserv[1]; // TODO:: PPKY_CONFIG_SIZE - все переменные
};

struct PPKYCfg {

	UniqId	UId;

	uint8_t beep; //

	MKUCfg	CfgDevices[32];

	int8_t zone_name[ZONE_NUMBER][ZONE_NAME_SIZE]; //
    /* . резерв нужен чтобы бесшовно обновлять устройство с имзенением структуры,
     * при этом резерв уменьшать на кол-во давленных новых данных
     */
    uint8_t reserv[1]; // TODO:: PPKY_CONFIG_SIZE - все переменные
};
//END PPKY

struct DeviceDPTConfig {
	/* Порог сопротивления для определения "Пожар" (Ом).
	 * По умолчанию 680 Ом.
	 * Если измеренное сопротивление <= fire_threshold_ohm, то состояние "Пожар"
	 */
	uint16_t fire_threshold_ohm;

	/* Порог сопротивления для определения "Норма" (Ом).
	 * По умолчанию 5380 Ом (680 + 4700).
	 * Если fire_threshold < сопротивление <= normal_threshold, то состояние "Норма"
	 */
	uint16_t normal_threshold_ohm;

	/* Порог сопротивления для определения "Обрыв" (Ом).
	 * По умолчанию 100000 Ом.
	 * Если сопротивление > break_threshold, то состояние "Обрыв"
	 */
	uint32_t break_threshold_ohm;

	/* Номинал резистора R1 в делителе напряжения (Ом).
	 * По умолчанию 10000 Ом (10 кОм).
	 * Используется для расчёта сопротивления линии по значениям АЦП.
	 */
	uint16_t resistor_r1_ohm;

	/* Номинал резистора R2 в делителе напряжения (Ом).
	 * По умолчанию 10000 Ом (10 кОм).
	 * Используется для расчёта сопротивления линии по значениям АЦП.
	 */
	uint16_t resistor_r2_ohm;

	/* Напряжение питания делителя (мВ).
	 * По умолчанию 3300 мВ (3.3 В).
	 * Используется для расчёта сопротивления линии.
	 */
	uint16_t supply_voltage_mv;

	/* Разрешение АЦП (максимальное значение).
	 * По умолчанию 4095 (12-битный АЦП).
	 */
	uint16_t adc_resolution;

	/* Режим \"концевик\".
	 * 0 - выключен: при срабатывании порога \"Пожар\" состояние = Fire.
	 * 1 - включен: при срабатывании порога \"Пожар\" состояние = Press (\"нажатие\").
	 */
	uint8_t  is_limit_switch;

	/* резерв для выравнивания под VDeviceCfg::reserv (0x100 байт) */
	uint8_t reserved[VDEVICE_CFG_SIZE - 17];
};


struct DeviceIgniterConfig {
	/* 0 - проверка КЗ включена (по умолчанию)
	 * 1 - проверка КЗ отключена, КЗ считается "Норма"
	 */
	uint8_t disable_sc_check;

	/* Длительность разгона ШИМ, мс.
	 * По умолчанию 1000 мс.
	 */
	uint16_t start_duration_ms;

	/* резерв для выравнивания под VDeviceCfg::reserv (0x100 байт) */
	uint8_t reserved[VDEVICE_CFG_SIZE - 3];
};




#endif /* INCLUDE_DEVICE_CONFIG_H_ */
