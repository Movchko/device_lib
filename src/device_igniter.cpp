
#include <device_igniter.hpp>

/* Фазы прожига: 0=разгон ШИМ, 1=удержание, 2=ожидание проверки, 3=проверка и решение */
#define BURN_PHASE_RAMP      0
#define BURN_PHASE_HOLD      1
#define BURN_PHASE_WAIT      2
#define BURN_PHASE_CHECK     3

#define BURN_RAMP_MS         200
#define BURN_HOLD_MS         800
#define BURN_WAIT_MS         50   /* пауза перед проверкой после сброса ШИМ */

VDeviceIgniter::VDeviceIgniter(uint8_t ChNum) : VDevice(ChNum) {
	State = DeviceIgniterState_Idle;
	Status = DeviceIgniterStatus_Idle;
	LineState = DeviceIgniterLineState_Normal;
	Config = nullptr;
	disable_sc_check = 0;
	threshold_break_low = 1000;
	threshold_break_high = 3000;
	burn_retry_count = 1;
	run_elapsed_ms = 0;
	pwm_value = 0;
	measured_resistance_ohm = 0;
	start_ack = 0;
	end_ack = 0;
	burn_phase = 0;
	burn_cycle = 0;
	debounce_candidate = 0;
	debounce_cnt = 0;
	pwm_off_cooldown_ms = 0;
	Counter1s = 0;
}

void VDeviceIgniter::Init() {
	if (CfgPtr != nullptr) {
		Config = reinterpret_cast<DeviceIgniterConfig*>(CfgPtr->reserv);

		if (Config->threshold_break_low == 0 && Config->threshold_break_high == 0) {
			Config->threshold_break_low  = 1000;
			Config->threshold_break_high = 3000;
			Config->burn_retry_count     = 1;
		}
	} else {
		Config = nullptr;
	}

	if (Config != nullptr) {
		if (Config->threshold_break_low == 0)  Config->threshold_break_low  = 1000;
		if (Config->threshold_break_high == 0) Config->threshold_break_high = 3000;
		threshold_break_low  = Config->threshold_break_low;
		threshold_break_high = Config->threshold_break_high;
		burn_retry_count     = Config->burn_retry_count;
		disable_sc_check     = Config->disable_sc_check ? 1 : 0;
	} else {
		threshold_break_low   = 1000;
		threshold_break_high  = 3000;
		burn_retry_count      = 1;
		disable_sc_check      = 0;
	}

	State = DeviceIgniterState_Idle;
	Status = DeviceIgniterStatus_Idle;
	LineState = DeviceIgniterLineState_Normal;
	run_elapsed_ms = 0;
	pwm_value = 0;
	measured_resistance_ohm = 0;
	start_ack = 0;
	end_ack = 0;
	burn_phase = 0;
	burn_cycle = 0;
	debounce_candidate = 0;
	debounce_cnt = 0;
	pwm_off_cooldown_ms = 0;
	UpdateStatus(DeviceIgniterStatus_Idle);
}

bool VDeviceIgniter::IsPwmActive() const {
	return (State == DeviceIgniterState_Run) &&
	       (burn_phase == BURN_PHASE_RAMP || burn_phase == BURN_PHASE_HOLD);
}

void VDeviceIgniter::UpdateLineFromAdcMv(uint16_t adc_mv) {
	/* Передаём текущее измеренное значение линии в статус (2 байта). */
	measured_resistance_ohm = adc_mv;
	if (IsPwmActive()) {
		return;  /* во время работы ШИМ значения АЦП невалидны */
	}
	if (pwm_off_cooldown_ms > 0) {
		return;  /* после выключения ШИМ — 100мс на установку напряжения */
	}
	///* adc_mv==0 — невалидно (старт АЦП, пустой фильтр), не меняем состояние */
	//if (adc_mv == 0) {
	//	return;
	//}
	/* 1..break_low = норма, break_low..break_high = обрыв/КЗ, >=break_high = ошибка */
	uint8_t new_candidate;
	if (adc_mv >= threshold_break_high) {
		new_candidate = (uint8_t)DeviceIgniterLineState_Break;  /* ошибка → обрыв */
	} else if (adc_mv < threshold_break_low) {
		new_candidate = (uint8_t)DeviceIgniterLineState_Normal;
	} else {
		new_candidate = (uint8_t)DeviceIgniterLineState_Break;   /* обрыв/КЗ */
	}

	if (new_candidate != debounce_candidate) {
		debounce_candidate = new_candidate;
		debounce_cnt = 0;
		return;
	}
	debounce_cnt++;
	if (debounce_cnt >= 100) {  /* 100 мс стабильно */
		debounce_cnt = 100;  /* не переполнять */
		DeviceIgniterLineState st = (DeviceIgniterLineState)new_candidate;
		SetLineState(st);
	}
}

void VDeviceIgniter::Process() {
	switch(State) {
		case DeviceIgniterState_Idle: {
			pwm_value = 0;
		} break;
		case DeviceIgniterState_Error: {
			pwm_value = 0;
		} break;
		case DeviceIgniterState_Run: {
			/* Во время разгона и удержания — реакция на обрыв/КЗ как ошибка.
			 * Только в первом цикле (burn_cycle==0): в повторах линия уже может быть оборвана. */
			//if ((burn_phase == BURN_PHASE_RAMP || burn_phase == BURN_PHASE_HOLD) && burn_cycle == 0) {
			//	HandleLineState();
			//}
			UpdatePwm();
		} break;
	}
}

void VDeviceIgniter::CommandCB(uint8_t Command, uint8_t *Parameters) {
	switch(Command) {
		case 10: {
			State = DeviceIgniterState_Run;
			Status = DeviceIgniterStatus_Run;
			run_elapsed_ms = 0;
			pwm_value = 0;
			burn_phase = BURN_PHASE_RAMP;
			burn_cycle = 0;
			start_ack = 1;
			end_ack = 0;
			UpdateStatus(Status);
#if 0
			if ((State == DeviceIgniterState_Idle) || (State == DeviceIgniterState_Error)) {
				if (LineState == DeviceIgniterLineState_Break) {
					State = DeviceIgniterState_Error;
					Status = DeviceIgniterStatus_Error;
					UpdateStatus(Status);
					return;
				}
				if ((LineState == DeviceIgniterLineState_Short) && (disable_sc_check == 0)) {
					State = DeviceIgniterState_Error;
					Status = DeviceIgniterStatus_Error;
					UpdateStatus(Status);
					return;
				}

				State = DeviceIgniterState_Run;
				Status = DeviceIgniterStatus_Run;
				run_elapsed_ms = 0;
				pwm_value = 0;
				burn_phase = BURN_PHASE_RAMP;
				burn_cycle = 0;
				start_ack = 1;
				end_ack = 0;
				UpdateStatus(Status);
			}
#endif
		} break;

		case 11: {
			uint8_t val = Parameters[0] ? 1 : 0;
			disable_sc_check = val;
			if (Config != nullptr) {
				Config->disable_sc_check = val;
			}
			if (VDeviceSaveCfg != nullptr) {
				VDeviceSaveCfg();
			}
		} break;

		/* 2 - команда установки порогов (мВ) и числа повторов
		 * Parameters[0,1] = threshold_break_low LE
		 * Parameters[2,3] = threshold_break_high LE
		 * Parameters[4]   = burn_retry_count (0 или 1)
		 */
		case 12: {
			if (Parameters != nullptr) {
				uint16_t bl = (uint16_t)Parameters[0] | ((uint16_t)Parameters[1] << 8);
				uint16_t bh = (uint16_t)Parameters[2] | ((uint16_t)Parameters[3] << 8);
				uint8_t rc = Parameters[4] > 1 ? 1 : Parameters[4];

				if (bl > 0) { threshold_break_low  = bl;  if (Config) Config->threshold_break_low  = bl; }
				if (bh > 0) { threshold_break_high = bh;  if (Config) Config->threshold_break_high = bh; }
				burn_retry_count = rc;
				if (Config) Config->burn_retry_count = rc;

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
	Data[0] = (uint8_t)LineState;
	Data[1] = (start_ack ? 0x01 : 0x00) | (end_ack ? 0x02 : 0x00);
	Data[2] = (uint8_t)(measured_resistance_ohm & 0xFFu);
	Data[3] = (uint8_t)((measured_resistance_ohm >> 8) & 0xFFu);
	VDeviceSetStatus(Num, Status, Data);
}

void VDeviceIgniter::Timer1ms() {
	if (Counter1s >= 1000) {
		SetStatus();
		Counter1s = 0;
	} else {
		Counter1s++;
	}

	if (pwm_off_cooldown_ms > 0) {
		pwm_off_cooldown_ms--;
	}

	Process();

	if (State == DeviceIgniterState_Run) {
		if (run_elapsed_ms < 0xFFFF) {
			run_elapsed_ms++;
		}
	}
}

void VDeviceIgniter::SetLineState(DeviceIgniterLineState st) {
	if ((st == DeviceIgniterLineState_Short) && (disable_sc_check != 0)) {
		st = DeviceIgniterLineState_Normal;
	}
	LineState = st;
	if (State == DeviceIgniterState_Run) {
		HandleLineState();
	}
}

void VDeviceIgniter::HandleLineState() {
	if (LineState == DeviceIgniterLineState_Break) {
		State = DeviceIgniterState_Error;
		Status = DeviceIgniterStatus_Error;
		pwm_value = 0;
		pwm_off_cooldown_ms = 100;  /* после аварийного выключения ШИМ — cooldown */
		UpdateStatus(Status);
		return;
	}
	if ((LineState == DeviceIgniterLineState_Short) && (disable_sc_check == 0)) {
		State = DeviceIgniterState_Error;
		Status = DeviceIgniterStatus_Error;
		pwm_value = 0;
		pwm_off_cooldown_ms = 100;
		UpdateStatus(Status);
		return;
	}
}

void VDeviceIgniter::UpdatePwm() {
	if (State != DeviceIgniterState_Run) {
		return;
	}
	//if (LineState != DeviceIgniterLineState_Normal) { // <<<< потом вернуть
	//	return;
	//}

	const uint16_t PWM_MAX = 99;

	switch (burn_phase) {
		case BURN_PHASE_RAMP: {
			if (run_elapsed_ms < BURN_RAMP_MS) {
				uint32_t val = (uint32_t)run_elapsed_ms * PWM_MAX;
				val /= BURN_RAMP_MS;
				if (val > PWM_MAX) val = PWM_MAX;
				pwm_value = (uint16_t)val;
			} else {
				pwm_value = PWM_MAX;
				burn_phase = BURN_PHASE_HOLD;
			}
		} break;

		case BURN_PHASE_HOLD: {
			pwm_value = PWM_MAX;
			if (run_elapsed_ms >= BURN_RAMP_MS + BURN_HOLD_MS) {
				pwm_value = 0;
				burn_phase = BURN_PHASE_WAIT;
				pwm_off_cooldown_ms = 100;  /* 100мс не доверять ADC после выключения ШИМ */
			}
		} break;

		case BURN_PHASE_WAIT: {
			pwm_value = 0;
			if (run_elapsed_ms >= BURN_RAMP_MS + BURN_HOLD_MS + BURN_WAIT_MS) {
				burn_phase = BURN_PHASE_CHECK;
			}
		} break;

		case BURN_PHASE_CHECK: {
			pwm_value = 0;
			/* end_ack выставляется только после всех попыток (всех циклов) */
			burn_cycle++;
			if (burn_cycle <= burn_retry_count) {
				/* Ещё есть попытки — повторный цикл */
				burn_phase = BURN_PHASE_RAMP;
				run_elapsed_ms = 0;
			} else {
				/* Все попытки выполнены — завершаем и выставляем end_ack */
				end_ack = 1;
				State = DeviceIgniterState_Idle;
				Status = DeviceIgniterStatus_Idle;
				UpdateStatus(Status);
			}
		} break;
	}
}
