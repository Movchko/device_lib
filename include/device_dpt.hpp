
#ifndef INCLUDE_DEVICE_DPT_HPP_
#define INCLUDE_DEVICE_DPT_HPP_

#include "device.hpp"
#include "device_config.h"

// пороги ниже при стабилитроне 4.3В при напряжении 24В

// в омах
// всё что выше - обрыв
#define DPT_LIMIT_BREAK 2000//6200
//  норма
#define DPT_LIMIT_NORMAL 820//4100
// неисправность
#define DPT_LIMIT_FAULT 160//910
//  пожар
#define DPT_LIMIT_FIRE 100//560
//  КЗ

#define TRY_24V_SHORT_MS 5000

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
	DeviceDPTLineState_Short  = 2,  /* КЗ (0 Ом) */
	DeviceDPTLineState_Fire   = 3,  /* Пожар (680 Ом) */
	DeviceDPTLineState_Press  = 4,  /* Нажатие / вскрытие концевика */
	DeviceDPTLineState_Fault  = 5   /* Неисправность */
};

/* Legacy-режим (исторически использовался одним классом для DPT/концевика/кнопки).
 * Теперь VDeviceDPT работает как "чистый ДПТ"; значения оставлены для совместимости config/tools.
 * 0 - ДПТ
 * 1 - концевик
 * 2 - кнопка
 */
/* Режим работы виртуального устройства:
 * 0 - ДПТ (пожар)
 * 1 - концевик (открытие)
 * 2 - кнопка (нажатие)
 */
enum DeviceDPTMode {
	DeviceDPTMode_DPT      = 0,
	DeviceDPTMode_Limit    = 1,
	DeviceDPTMode_Button   = 2
};

class VDeviceDPT: public VDevice {
	DeviceDPTState State;
	DeviceDPTStatus Status;
	void UpdateStatus(DeviceDPTStatus status);
	uint32_t Counter1s;

	DeviceDPTLineState prevLineState, LineState;
	DeviceDPTConfig *Config;

	/* Режим устройства (ДПТ / концевик / кнопка) */
	DeviceDPTMode Mode;

	/* Порог для MAX (°C) и задержка смены состояния (мс) */
	uint16_t max_fire_threshold_c;
	uint16_t state_change_delay_ms;

	/* Данные MAX: последняя температура и флаг неисправности */
	int16_t max_temp_c;
	uint8_t max_fault;
	int16_t max_internal_temp_c;

	/* Текущие значения АЦП каналов */
	uint16_t adc_ch1_value;
	uint16_t adc_ch2_value;

	/* Текущее измеренное сопротивление линии (Ом) */
	uint32_t measured_resistance_ohm;

	/* Фильтр по времени для смены состояния */
	DeviceDPTLineState pendingLineState;
	uint16_t pendingTimeMs;

	/* Режим измерения: по сопротивлению или по MAX */
	uint8_t measureModeIsMax;

	uint8_t useMax;

	/* Таймер: ожидание стабилизации MAX после переключения, мс */
	uint16_t maxSettleMs;

	/* Режим пробного включения 24В после КЗ и его таймер (мс) */
	uint8_t  probeAfterShort;
	uint16_t probeTimerMs;


	uint16_t maxRetryTimerMs;

	uint8_t was_fire; // был пожар

	void UpdateLineStateInstant();
	void UpdateLineStateFiltered();
protected:
	/* Как интерпретировать "сработку" уровня (по умолчанию для DPT = Fire). */
	virtual DeviceDPTLineState GetTriggeredLineState() const;

public:
	VDeviceDPT(uint8_t ChNum);

	uint8_t GetDT();
	void Init();
	void Process();
	void CommandCB(uint8_t Command, uint8_t *Parameters);
	void SetStatus();
	void Timer1ms();

	/* Вызывается из внешнего кода.
	 * ch1 — уже пересчитанное сопротивление в Омах,
	 * ch2 — резерв / отладка.
	 */
	void SetAdcValues(uint16_t ch1, uint16_t ch2);

	/* Обновление данных MAX (термопара, битовая маска fault, внутренняя температура). */
	void SetMaxStatus(int16_t temp_c, uint8_t fault_mask, int16_t internal_temp_c);

	/* Функции управления питанием/режимом измерения ДПТ (устанавливаются в MCU_TC/app.cpp) */
	void (*DPT_SetResMeasureMode)() = nullptr;
	void (*DPT_SetMaxMeasureMode)() = nullptr;

	/* Получить текущее состояние линии */
	DeviceDPTLineState GetLineState() const { return LineState; }

	/* Получить измеренное сопротивление (Ом) */
	uint32_t GetMeasuredResistance() const { return measured_resistance_ohm; }
};

#endif /* INCLUDE_DEVICE_DPT_HPP_ */





