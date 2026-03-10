
#ifndef INCLUDE_DEVICE_IGNITER_HPP_
#define INCLUDE_DEVICE_IGNITER_HPP_

#include "device.hpp"
#include "device_config.h"





enum DeviceIgniterState {
	DeviceIgniterState_Idle,
	DeviceIgniterState_Run,
	DeviceIgniterState_Error
};

enum DeviceIgniterStatus {
	DeviceIgniterStatus_Idle,
	DeviceIgniterStatus_Run,
	DeviceIgniterStatus_Error
};

/* Состояние линии воспламенителя */
enum DeviceIgniterLineState {
	DeviceIgniterLineState_Normal = 0,
	DeviceIgniterLineState_Break  = 1,
	DeviceIgniterLineState_Short  = 2,
};

class VDeviceIgniter: public VDevice {
	DeviceIgniterState State;
	DeviceIgniterStatus Status;
	void UpdateStatus(DeviceIgniterStatus status);
	uint32_t Counter1s;

	DeviceIgniterLineState LineState;
	DeviceIgniterConfig *Config;

	/* флаг: 1 - игнорировать КЗ, считать как норму */
	uint8_t disable_sc_check;

	/* пороги ADC (мВ) и число повторов прожига */
	uint16_t threshold_break_low;
	uint16_t threshold_break_high;
	uint8_t burn_retry_count;

	/* внутренний счётчик времени в Run, мс */
	uint16_t run_elapsed_ms;
	/* фаза прожига: 0=разгон, 1=удержание, 2=проверка, 3=повтор */
	uint8_t burn_phase;
	uint8_t burn_cycle;  /* номер цикла (0 или 1) */

	/* дебаунс 100мс для состояния линии по ADC */
	uint8_t debounce_candidate;
	uint8_t debounce_cnt;

	/* после выключения ШИМ — не доверять ADC 100мс (установка напряжения) */
	uint8_t pwm_off_cooldown_ms;

	/* текущий уровень ШИМ (0..PWM_MAX) */
	uint16_t pwm_value;

	/* флаги подтверждений */
	uint8_t start_ack;
	uint8_t end_ack;

	void HandleLineState();
	void UpdatePwm();

public:
	VDeviceIgniter(uint8_t ChNum);

	uint8_t GetDT() {return DEVICE_IGNITER_TYPE;}
	void Init();
	void Process();
	void CommandCB(uint8_t Command, uint8_t *Parameters);
	void SetStatus();
	void Timer1ms();

	/* Вызывается из внешнего кода при изменении/опросе линии */
	void SetLineState(DeviceIgniterLineState st);

	/* Обновление состояния линии по ADC (мВ), с дебаунсом 100мс.
	 * Вызывать только когда ШИМ выключен — во время работы ШИМ значения АЦП невалидны. */
	void UpdateLineFromAdcMv(uint16_t adc_mv);

	/* true = ШИМ активен (разгон или удержание), ADC невалиден */
	bool IsPwmActive() const;

	/* Текущий уровень ШИМ (для низкоуровневого драйвера) */
	uint16_t GetPwm() const { return pwm_value; }
};

#endif /* INCLUDE_DEVICE_IGNITER_HPP_ */
