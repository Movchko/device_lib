
#include <device_dpt.hpp>
#include <string.h>

/* Временно отключаем влияние max_fault на классификацию линии.
 * 0 - игнорировать max_fault в логике (но передавать в статусе)
 * 1 - учитывать max_fault как DeviceDPTLineState_Fault */
#ifndef DPT_USE_MAX_FAULT_IN_LOGIC
#define DPT_USE_MAX_FAULT_IN_LOGIC 0
#endif

VDeviceDPT::VDeviceDPT(uint8_t ChNum) : VDevice(ChNum) {
	State = DeviceDPTState_Idle;
	Status = DeviceDPTStatus_Idle;
	prevLineState = LineState = DeviceDPTLineState_Normal;
	Config = nullptr;
	Mode = DeviceDPTMode_DPT;
	max_fire_threshold_c = 60;      /* порог MAX по умолчанию, °C */
	state_change_delay_ms = 100;    /* фильтр по времени по умолчанию */
	adc_ch1_value = 0;
	adc_ch2_value = 0;
	measured_resistance_ohm = 0;
	Counter1s = 0;
	pendingLineState = LineState;
	pendingTimeMs = 0;
	max_temp_c = 0;
	max_fault = 0;
	max_internal_temp_c = 0;
	measureModeIsMax = 0;
	useMax = 0;
	maxRetryTimerMs = 0;
	maxSettleMs = 0;
	probeAfterShort = 0;
	probeTimerMs = 0;
	was_fire = 0;
}

DeviceDPTLineState VDeviceDPT::GetTriggeredLineState() const {
	return DeviceDPTLineState_Fire;
}

void VDeviceDPT::Init() {
	/* Привязка конфигурации к общему буферу VDeviceCfg::reserv */
	if (CfgPtr != nullptr) {
		Config = reinterpret_cast<DeviceDPTConfig*>(CfgPtr->reserv);
	} else {
		Config = nullptr;
	}

	if (Config != nullptr) {
		/* mode оставляем как legacy-параметр в конфиге, но на поведение "чистого ДПТ" не влияет */
		Mode = DeviceDPTMode_DPT;

		useMax = Config->use_max ? 1u : 0u;

		if (Config->max_fire_threshold_c != 0u) {
			max_fire_threshold_c = Config->max_fire_threshold_c;
		} else {
			max_fire_threshold_c = 60u;
		}

		if (Config->state_change_delay_ms != 0u) {
			state_change_delay_ms = Config->state_change_delay_ms;
		} else {
			state_change_delay_ms = 100u;
		}
	} else {
		Mode = DeviceDPTMode_DPT;
		useMax = 1u;
		max_fire_threshold_c = 60u;
		state_change_delay_ms = 100u;
	}

	State = DeviceDPTState_Idle;
	Status = DeviceDPTStatus_Idle;
	LineState = DeviceDPTLineState_Normal;
	pendingLineState = LineState;
	pendingTimeMs = 0;
	adc_ch1_value = 0;
	adc_ch2_value = 0;
	measured_resistance_ohm = 0;
	measureModeIsMax = 0;
	max_internal_temp_c = 0;
	maxRetryTimerMs = 0;
	maxSettleMs = 0;
	probeAfterShort = 0;
	probeTimerMs = 0;
	UpdateStatus(DeviceDPTStatus_Idle);
}

void VDeviceDPT::Process() {
	switch(State) {
		case DeviceDPTState_Idle: {
			/* В режиме Idle просто обновляем состояние линии */
			UpdateLineStateFiltered();
		} break;

		case DeviceDPTState_Error: {
			/* В режиме Error тоже проверяем линию (может восстановиться) */
			UpdateLineStateFiltered();
		} break;
	}
}

void VDeviceDPT::CommandCB(uint8_t Command, uint8_t *Parameters) {
	switch(Command) {
		case 12: {
			/* Установка порога MAX (°C) (младший байт, старший байт) */
			if (Config != nullptr && Parameters != nullptr) {
				uint16_t threshold = Parameters[0] | (Parameters[1] << 8);
				if (threshold > 0) {
					Config->max_fire_threshold_c = threshold;
					max_fire_threshold_c = threshold;
					if (VDeviceSaveCfg != nullptr) {
						VDeviceSaveCfg();
					}
				}
			}
		} break;

		case 13: {
			/* Установка времени стабилизации уровня (младший байт, старший байт), мс */
			if (Config != nullptr && Parameters != nullptr) {
				uint16_t threshold = Parameters[0] | (Parameters[1] << 8);
				if (threshold > 0) {
					Config->state_change_delay_ms = threshold;
					state_change_delay_ms = threshold;
					if (VDeviceSaveCfg != nullptr) {
						VDeviceSaveCfg();
					}
				}
			}
		} break;

		case 14: {
			/* Legacy: сохраняем mode в конфиг для совместимости, но логика DPT от него не зависит */
			if (Config != nullptr && Parameters != nullptr) {
				uint8_t mode = Parameters[0];
				if (mode <= DeviceDPTMode_Button) {
					Config->mode = mode;
					Mode = DeviceDPTMode_DPT;
					if (VDeviceSaveCfg != nullptr) {
						VDeviceSaveCfg();
					}
				}
			}
		} break;

		default: {
			/* Неизвестная команда - игнорируем */
		} break;
	}
}

void VDeviceDPT::UpdateStatus(DeviceDPTStatus status) {
	Status = status;
	SetStatus();
}

void VDeviceDPT::SetStatus() {
	uint8_t Data[7] = {0, 0, 0, 0, 0, 0, 0};

	// если был пожар, не убираем статус пожара до перезагрузки
	if(was_fire)
		LineState = DeviceDPTLineState_Fire;

	if(LineState == DeviceDPTLineState_Fire)
		was_fire = 1;



	/* Data[0] - состояние линии:
	 * 0 - Норма
	 * 1 - Обрыв
	 * 2 - КЗ
	 * 3 - Пожар
	 * 4 - Нажатие
	 */

	Data[0] = LineState;

	/* Data[1..2] - измеренное сопротивление (младший байт, старший байт), Ом */
	Data[1] = measured_resistance_ohm & 0xFF;
	Data[2] = (measured_resistance_ohm >> 8) & 0xFF;


	/* Data[3] - температура термопары MAX (int8)
	 * Data[4] - fault bitmask MAX
	 * Data[5] - внутренняя температура MAX (int8) */
	if(max_temp_c > 0xff)
		Data[3] = 0xff;
	else
		Data[3] = max_temp_c & 0xFF;
	Data[4] = max_fault & 0xFF;
	Data[5] = max_internal_temp_c & 0xFF;

	if (VDeviceSetStatus != nullptr) {
		VDeviceSetStatus(Num, Status, Data);
	}
}

void VDeviceDPT::Timer1ms() {
    Counter1s++;
    if (Counter1s >= 1000) {
        SetStatus();
        Counter1s = 0;
    }

    /* Обновляем состояние линии (по сопротивлению или по MAX, в зависимости от measureModeIsMax/useMax) */
    Process();  // внутри Process -> UpdateLineStateFiltered -> prevLineState

    /* Окно стабилизации после пробного включения 24В */
    if (probeAfterShort) {
        const uint16_t PROBE_SETTLE_MS = 500;
        if (probeTimerMs < PROBE_SETTLE_MS) {
            probeTimerMs++;
            /* В течение окна 500 мс не переключаемся обратно на MAX,
             * даём АЦП/фильтру увидеть, что КЗ ушёл.
             */
            return;
        } else {
            probeAfterShort = 0;
            probeTimerMs = 0;
        }
    }

    /* Логика работы с КЗ и MAX */

    if (!useMax) {
        /* 1. MAX не используется: только включаем/отключаем 24В раз в 3 с при КЗ */

        if (prevLineState == DeviceDPTLineState_Short) {
            /* Первый вход в КЗ — сразу снимаем 24В */
            if (maxRetryTimerMs == 0) {
                if (DPT_SetMaxMeasureMode) {
                    /* Используем как "24В OFF".
                     * Если хочешь без щёлкания реле — скорректируй реализацию App_DPT_SetMaxMeasureMode.
                     */
                    DPT_SetMaxMeasureMode();
                }
            }

            if (maxRetryTimerMs < TRY_24V_SHORT_MS) {
                maxRetryTimerMs++;
            } else {
                /* Каждые 3 секунды пробуем снова подать 24В и измерить сопротивление */
                maxRetryTimerMs = 0;
                if (DPT_SetResMeasureMode) {
                    probeAfterShort = 1;
                    probeTimerMs = 0;
                    DPT_SetResMeasureMode();  // кратко включаем 24В, измерение дальше по ADC
                }
            }
        } else {
            maxRetryTimerMs = 0;
        }

    } else {
        /* 2. MAX используется (useMax=1) */

        /* Переход в режим MAX при устойчивом КЗ (только вне окна пробы) */
        if (!measureModeIsMax && !probeAfterShort && prevLineState == DeviceDPTLineState_Short) {
            measureModeIsMax = 1;
            maxRetryTimerMs = 0;
            maxSettleMs = 0;
            if (DPT_SetMaxMeasureMode) {
                /* 24В OFF, реле на MAX */
                DPT_SetMaxMeasureMode();
            }
        }

        if (measureModeIsMax) {
            /* Дать MAX стабилизироваться после переключения реле */
            const uint16_t MAX_SETTLE_TIME_MS = 2000;
            if (maxSettleMs < MAX_SETTLE_TIME_MS) {
                maxSettleMs++;
                /* Пока MAX не устаканился — не выходим из режима MAX,
                 * считаем, что линия по-прежнему в состоянии КЗ.
                 */
                return;
            }

            /* Мы сейчас в режиме MAX (классификация по max_temp_c/max_fault) */

            if (prevLineState != DeviceDPTLineState_Short) {
                /* По MAX линия перестала быть КЗ → можно вернуться к сопротивлению */
                measureModeIsMax = 0;
                maxRetryTimerMs = 0;
                maxSettleMs = 0;
                if (DPT_SetResMeasureMode) {
                    DPT_SetResMeasureMode();  // вернуть 24В и измерение сопротивления
                }
            } else {
                /* Линия по-прежнему считается КЗ → раз в 3 с пробуем включить 24В и померить R */
                if (maxRetryTimerMs < TRY_24V_SHORT_MS) {
                    maxRetryTimerMs++;
                } else {
                    maxRetryTimerMs = 0;
                    /* Краткий выход в режим сопротивления для проверки КЗ */
                    measureModeIsMax = 0;
                    probeAfterShort = 1;
                    probeTimerMs = 0;
                    if (DPT_SetResMeasureMode) {
                        DPT_SetResMeasureMode(); // 24В ON, реле на сопротивление
                    }
                    /* После этого ADC обновится, UpdateLineStateFiltered опять даст либо Short, либо другое состояние;
                     * при повторном устойчивом КЗ снова войдём в блок выше и вернёмся к MAX.
                     */
                }
            }
        } else {
            /* Вне режима MAX, MAX включён, но нет КЗ — просто сбрасываем таймер */
            if (prevLineState != DeviceDPTLineState_Short) {
                maxRetryTimerMs = 0;
                maxSettleMs = 0;
            }
        }
    }
}

void VDeviceDPT::SetAdcValues(uint16_t ch1, uint16_t ch2) {
	adc_ch1_value = ch1;
	adc_ch2_value = ch2;

	/* В ch1 уже приходит сопротивление линии в Омах */
	measured_resistance_ohm = ch1;

	/* Обновляем состояние линии на основе измеренного сопротивления */
	UpdateLineStateFiltered();
}

void VDeviceDPT::SetMaxStatus(int16_t temp_c, uint8_t fault, int16_t internal_temp_c) {
	max_temp_c = temp_c;
	max_fault = fault;
	max_internal_temp_c = internal_temp_c;
	/* В режиме MAX классификация состояния идёт по данным MAX */
	if (measureModeIsMax) {
		UpdateLineStateFiltered();
	}
}

void VDeviceDPT::UpdateLineStateInstant() {
	uint32_t r = measured_resistance_ohm;
	if (!measureModeIsMax) {
		/* Классификация по сопротивлению */
		if (r > DPT_LIMIT_BREAK) {
			LineState = DeviceDPTLineState_Break;
		} else if (r >= DPT_LIMIT_NORMAL) {
			LineState = DeviceDPTLineState_Normal;
		} else if (r >= DPT_LIMIT_FAULT) {
			LineState = DeviceDPTLineState_Fault;
		} else if (r >= DPT_LIMIT_FIRE) {
			/* Для DPT с MAX: в режиме сопротивления fire не выставляем до подтверждения по MAX. */
			if (useMax) {
				LineState = DeviceDPTLineState_Short;
			} else {
				LineState = GetTriggeredLineState();
			}
		} else {
			LineState = DeviceDPTLineState_Short;
		}
	} else {
		/* Классификация по MAX:
		 * fault != 0        → неисправность
		 * !fault && T>порог → пожар
		 * иначе             → КЗ без пожара
		 */
		if (DPT_USE_MAX_FAULT_IN_LOGIC && max_fault) {
			LineState = DeviceDPTLineState_Fault;
		} else if (max_temp_c > static_cast<int16_t>(max_fire_threshold_c)) {
			LineState = GetTriggeredLineState();
		} else {
			LineState = DeviceDPTLineState_Short;
		}
	}
}

void VDeviceDPT::UpdateLineStateFiltered() {

	/* Кандидат в новое состояние по текущему измерению */
	UpdateLineStateInstant();
	DeviceDPTLineState candidate = LineState;

	if (candidate != prevLineState) {
		if (pendingLineState != candidate) {
			pendingLineState = candidate;
			pendingTimeMs = 0;
		} else {
			if (pendingTimeMs < state_change_delay_ms) {
				pendingTimeMs++;
			}
			if (pendingTimeMs >= state_change_delay_ms) {
				prevLineState = candidate;
				LineState = candidate;
				SetStatus();
			}
		}
	} else {
		/* Состояние не меняется — сбрасываем фильтр */
		pendingLineState = prevLineState;
		pendingTimeMs = 0;
		LineState = prevLineState;
	}
}

uint8_t VDeviceDPT::GetDT() {
	return DEVICE_DPT_TYPE;
}
