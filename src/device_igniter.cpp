
#include <device_igniter.hpp>

VDeviceIgniter::VDeviceIgniter(uint8_t ChNum) : VDevice(ChNum) {
	State = DeviceIgniterState_Idle;
	Status = DeviceIgniterStatus_Idle;
	LineState = DeviceIgniterLineState_Normal;
	Config = nullptr;
	disable_sc_check = 0;
	start_duration_ms = 1000;
	run_elapsed_ms = 0;
	pwm_value = 0;
	start_ack = 0;
	end_ack = 0;
	Counter1s = 0;
}

void VDeviceIgniter::Init() {
	/* Привязка конфигурации к общему буферу VDeviceCfg::reserv (как в VDeviceDPT) */
	if (CfgPtr != nullptr) {
		Config = reinterpret_cast<DeviceIgniterConfig*>(CfgPtr->reserv);

		/* Если конфиг "пустой" – устанавливаем значения по умолчанию */
		if (Config->start_duration_ms == 0) {
			Config->disable_sc_check  = 0;
			Config->start_duration_ms = 1000;
		}
	} else {
		Config = nullptr;
	}

	if (Config != nullptr) {
		if (Config->start_duration_ms == 0) {
			Config->start_duration_ms = 1000;
		}
		start_duration_ms = Config->start_duration_ms;
		disable_sc_check = Config->disable_sc_check ? 1 : 0;
	} else {
		start_duration_ms = 1000;
		disable_sc_check = 0;
	}

	State = DeviceIgniterState_Idle;
	Status = DeviceIgniterStatus_Idle;
	LineState = DeviceIgniterLineState_Normal;
	run_elapsed_ms = 0;
	pwm_value = 0;
	start_ack = 0;
	end_ack = 0;
	UpdateStatus(DeviceIgniterStatus_Idle);
}

void VDeviceIgniter::Process() {
    switch(State) {
		case DeviceIgniterState_Idle: {
			/* В режиме ожидания держим ШИМ в нуле */
			pwm_value = 0;
		} break;
		case DeviceIgniterState_Error: {
			/* В ошибке ШИМ всегда выключен */
			pwm_value = 0;
		} break;
		case DeviceIgniterState_Run: {
			HandleLineState();
			UpdatePwm();
		}break;
    }
}

void VDeviceIgniter::CommandCB(uint8_t Command, uint8_t *Parameters) {
    switch(Command) {
		/* 0 - команда "Запуск" */
		case 0: {
			/* Запуск разрешён только из Idle или Error (восстановление) */
			if ((State == DeviceIgniterState_Idle) || (State == DeviceIgniterState_Error)) {
				/* Если линия в обрыве - не стартуем */
				if (LineState == DeviceIgniterLineState_Break) {
					State = DeviceIgniterState_Error;
					Status = DeviceIgniterStatus_Error;
					UpdateStatus(Status);
					return;
				}

				/* Если КЗ и проверка не отключена - тоже ошибка */
				if ((LineState == DeviceIgniterLineState_Short) && (disable_sc_check == 0)) {
					State = DeviceIgniterState_Error;
					Status = DeviceIgniterStatus_Error;
					UpdateStatus(Status);
					return;
				}

				/* Старт */
				State = DeviceIgniterState_Run;
				Status = DeviceIgniterStatus_Run;
				run_elapsed_ms = 0;
				pwm_value = 0;
				start_ack = 1;
				end_ack = 0;
				UpdateStatus(Status);
			}
		} break;

		/* 1 - команда конфигурации (вкл/выкл проверки КЗ)
		 * Parameters[0] = 0 - проверка включена
		 * Parameters[0] = 1 - проверка выключена
		 */
		case 1: {
			uint8_t val = Parameters[0] ? 1 : 0;
			disable_sc_check = val;
			if (Config != nullptr) {
				Config->disable_sc_check = val;
			}
			if (VDeviceSaveCfg != nullptr) {
				VDeviceSaveCfg();
			}
		} break;

		/* 2 - команда установки времени разгона (мс), 16 бит, little-endian */
		case 2: {
			if (Parameters != nullptr) {
				uint16_t dur = (uint16_t)Parameters[0] | ((uint16_t)Parameters[1] << 8);
				if (dur == 0) dur = 1000;
				start_duration_ms = dur;
				if (Config != nullptr) {
					Config->start_duration_ms = start_duration_ms;
				}
				if (VDeviceSaveCfg != nullptr) {
					VDeviceSaveCfg();
				}
			}
		} break;
    }
}


void VDeviceIgniter::UpdateStatus(DeviceIgniterStatus status) {
    Status = status;
    SetStatus();
}

void VDeviceIgniter::SetStatus() {
	uint8_t Data[7] = {0, 0, 0, 0, 0, 0, 0};

	/* Data[0] - состояние линии
	 * 0 - Норма
	 * 1 - Обрыв
	 * 2 - КЗ
	 */
	Data[0] = (uint8_t)LineState;

	/* Data[1] - флаги подтверждений
	 * bit0 - старт подтверждён
	 * bit1 - окончание разгона
	 */
	Data[1] = (start_ack ? 0x01 : 0x00) | (end_ack ? 0x02 : 0x00);

	VDeviceSetStatus(Num, Status, Data);
}

void VDeviceIgniter::Timer1ms() {
	if(Counter1s >= 1000) {
		SetStatus();
		Counter1s = 0;
	} else {
		Counter1s++;
	}

	Process();

	/* счётчик времени работы в режиме Run */
	if (State == DeviceIgniterState_Run) {
		if (run_elapsed_ms < 0xFFFF) {
			run_elapsed_ms++;
		}
	}
}

void VDeviceIgniter::SetLineState(DeviceIgniterLineState st) {
	/* Если проверка КЗ отключена - считаем КЗ нормой */
	if ((st == DeviceIgniterLineState_Short) && (disable_sc_check != 0)) {
		st = DeviceIgniterLineState_Normal;
	}

	LineState = st;

	/* Мгновенная реакция на обрыв/КЗ в режиме Run */
	if (State == DeviceIgniterState_Run) {
		HandleLineState();
	}
}

void VDeviceIgniter::HandleLineState() {
	/* Обрыв - всегда ошибка, выключаем ШИМ */
	if (LineState == DeviceIgniterLineState_Break) {
		State = DeviceIgniterState_Error;
		Status = DeviceIgniterStatus_Error;
		pwm_value = 0;
		UpdateStatus(Status);
		return;
	}

	/* КЗ при включённой проверке - ошибка */
	if ((LineState == DeviceIgniterLineState_Short) && (disable_sc_check == 0)) {
		State = DeviceIgniterState_Error;
		Status = DeviceIgniterStatus_Error;
		pwm_value = 0;
		UpdateStatus(Status);
		return;
	}
}

void VDeviceIgniter::UpdatePwm() {
	/* В режиме Run, при нормальной линии, разгоняем ШИМ до конца интервала */
	if (State != DeviceIgniterState_Run) {
		return;
	}

	/* Если линия не нормальная - HandleLineState() уже перевёл в ошибку */
	if (LineState != DeviceIgniterLineState_Normal) {
		return;
	}

	const uint16_t PWM_MAX = 99; /* под TIM с Period = 99 */

	if (run_elapsed_ms < start_duration_ms) {
		/* Линейный разгон от 0 до PWM_MAX за start_duration_ms */
		uint32_t val = (uint32_t)run_elapsed_ms * PWM_MAX;
		val /= start_duration_ms;
		if (val > PWM_MAX) val = PWM_MAX;
		pwm_value = (uint16_t)val;
	} else {
		/* Достигли конца разгона - держим максимальный ШИМ */
		pwm_value = PWM_MAX;
		if (!end_ack) {
			end_ack = 1;
			UpdateStatus(Status);
		}
		if(run_elapsed_ms >= 5000) { /* если 5 секунд жгли и не прожшли */
			State = DeviceIgniterState_Error;
			Status = DeviceIgniterStatus_Error;
			pwm_value = 0;
			UpdateStatus(Status);
		}
	}
}
