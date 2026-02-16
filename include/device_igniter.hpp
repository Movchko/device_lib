
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

	/* время разгона ШИМ, мс */
	uint16_t start_duration_ms;

	/* внутренний счётчик времени запуска, мс */
	uint16_t run_elapsed_ms;

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

	/* Текущий уровень ШИМ (для низкоуровневого драйвера) */
	uint16_t GetPwm() const { return pwm_value; }
};

#endif /* INCLUDE_DEVICE_IGNITER_HPP_ */
