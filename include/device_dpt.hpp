
#ifndef INCLUDE_DEVICE_DPT_HPP_
#define INCLUDE_DEVICE_DPT_HPP_

#include "device.hpp"
#include "device_config.h"



enum DeviceDPTState {
	DeviceDPTState_Idle,
	DeviceDPTState_Error
};

enum DeviceDPTStatus {
	DeviceDPTStatus_Idle,
	DeviceDPTStatus_Error
};

/* Состояние линии датчика ДПТ */
enum DeviceDPTLineState {
	DeviceDPTLineState_Normal = 0,  /* Норма (680+4700 Ом) */
	DeviceDPTLineState_Break  = 1,  /* Обрыв */
	DeviceDPTLineState_Short  = 2, /* КЗ (0 Ом) */
	DeviceDPTLineState_Fire   = 3,  /* Пожар (680 Ом) */
	DeviceDPTLineState_Press  = 4   /* Нажатие / вскрытие концевика */
};

class VDeviceDPT: public VDevice {
	DeviceDPTState State;
	DeviceDPTStatus Status;
	void UpdateStatus(DeviceDPTStatus status);
	uint32_t Counter1s;

	DeviceDPTLineState prevLineState, LineState;
	DeviceDPTConfig *Config;

	/* Пороги сопротивления (из конфига) */
	uint16_t fire_threshold_ohm;
	uint16_t normal_threshold_ohm;
	uint32_t break_threshold_ohm;

	/* Параметры делителя напряжения */
	uint16_t resistor_r1_ohm;
	uint16_t resistor_r2_ohm;
	uint16_t supply_voltage_mv;
	uint16_t adc_resolution;

	/* Флаг режима концевика */
	uint8_t  is_limit_switch;

	/* Текущие значения АЦП каналов */
	uint16_t adc_ch1_value;
	uint16_t adc_ch2_value;

	/* Текущее измеренное сопротивление линии (Ом) */
	uint32_t measured_resistance_ohm;

	void UpdateLineState();
	uint32_t CalculateResistance(uint16_t adc_value);

public:
	VDeviceDPT(uint8_t ChNum);

	uint8_t GetDT() {return DEVICE_DPT_TYPE;}
	void Init();
	void Process();
	void CommandCB(uint8_t Command, uint8_t *Parameters);
	void SetStatus();
	void Timer1ms();

	/* Вызывается из внешнего кода для установки значений АЦП каналов */
	void SetAdcValues(uint16_t ch1, uint16_t ch2);

	/* Получить текущее состояние линии */
	DeviceDPTLineState GetLineState() const { return LineState; }

	/* Получить измеренное сопротивление (Ом) */
	uint32_t GetMeasuredResistance() const { return measured_resistance_ohm; }
};

#endif /* INCLUDE_DEVICE_DPT_HPP_ */





