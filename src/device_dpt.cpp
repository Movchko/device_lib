
#include <device_dpt.hpp>
#include <string.h>

VDeviceDPT::VDeviceDPT(uint8_t ChNum) : VDevice(ChNum) {
	State = DeviceDPTState_Idle;
	Status = DeviceDPTStatus_Idle;
	prevLineState = LineState = DeviceDPTLineState_Normal;
	Config = nullptr;
	fire_threshold_ohm = 680;
	normal_threshold_ohm = 5380;
	break_threshold_ohm = 100000;
	resistor_r1_ohm = 10000;
	resistor_r2_ohm = 10000;
	supply_voltage_mv = 3300;
	adc_resolution = 4095;
	is_limit_switch = 0;
	adc_ch1_value = 0;
	adc_ch2_value = 0;
	measured_resistance_ohm = 0;
	Counter1s = 0;
}

void VDeviceDPT::Init() {
	/* Привязка конфигурации к общему буферу VDeviceCfg::reserv */
	if (CfgPtr != nullptr) {
		Config = reinterpret_cast<DeviceDPTConfig*>(CfgPtr->reserv);
		/* Если конфиг пустой (все нули), устанавливаем значения по умолчанию */
		if (Config->fire_threshold_ohm == 0 && Config->normal_threshold_ohm == 0) {
			Config->fire_threshold_ohm = 680;
			Config->normal_threshold_ohm = 5380;
			Config->break_threshold_ohm = 100000;
			Config->resistor_r1_ohm = 10000;
			Config->resistor_r2_ohm = 10000;
			Config->supply_voltage_mv = 3300;
			Config->adc_resolution = 4095;
			Config->is_limit_switch = 0;
		}
	} else {
		Config = nullptr;
	}

	if (Config != nullptr) {
		/* Загружаем пороги из конфига */
		if (Config->fire_threshold_ohm == 0) {
			Config->fire_threshold_ohm = 680;
		}
		fire_threshold_ohm = Config->fire_threshold_ohm;

		if (Config->normal_threshold_ohm == 0) {
			Config->normal_threshold_ohm = 5380;
		}
		normal_threshold_ohm = Config->normal_threshold_ohm;

		if (Config->break_threshold_ohm == 0) {
			Config->break_threshold_ohm = 100000;
		}
		break_threshold_ohm = Config->break_threshold_ohm;

		/* Загружаем параметры делителя */
		if (Config->resistor_r1_ohm == 0) {
			Config->resistor_r1_ohm = 10000;
		}
		resistor_r1_ohm = Config->resistor_r1_ohm;

		if (Config->resistor_r2_ohm == 0) {
			Config->resistor_r2_ohm = 10000;
		}
		resistor_r2_ohm = Config->resistor_r2_ohm;

		if (Config->supply_voltage_mv == 0) {
			Config->supply_voltage_mv = 3300;
		}
		supply_voltage_mv = Config->supply_voltage_mv;

		if (Config->adc_resolution == 0) {
			Config->adc_resolution = 4095;
		}
		adc_resolution = Config->adc_resolution;

		/* Режим концевика */
		is_limit_switch = Config->is_limit_switch ? 1 : 0;
	} else {
		/* Значения по умолчанию */
		fire_threshold_ohm = 680;
		normal_threshold_ohm = 5380;
		break_threshold_ohm = 100000;
		resistor_r1_ohm = 10000;
		resistor_r2_ohm = 10000;
		supply_voltage_mv = 3300;
		adc_resolution = 4095;
		is_limit_switch = 0;
	}

	State = DeviceDPTState_Idle;
	Status = DeviceDPTStatus_Idle;
	LineState = DeviceDPTLineState_Normal;
	adc_ch1_value = 0;
	adc_ch2_value = 0;
	measured_resistance_ohm = 0;
	UpdateStatus(DeviceDPTStatus_Idle);
}

void VDeviceDPT::Process() {
	switch(State) {
		case DeviceDPTState_Idle: {
			/* В режиме Idle просто обновляем состояние линии */
			UpdateLineState();
		} break;

		case DeviceDPTState_Error: {
			/* В режиме Error тоже проверяем линию (может восстановиться) */
			UpdateLineState();
		} break;
	}
}

void VDeviceDPT::CommandCB(uint8_t Command, uint8_t *Parameters) {
	switch(Command) {
		case 2: {
			/* Установка порога "Пожар" (младший байт, старший байт) */
			if (Config != nullptr && Parameters != nullptr) {
				uint16_t threshold = Parameters[0] | (Parameters[1] << 8);
				if (threshold > 0) {
					Config->fire_threshold_ohm = threshold;
					fire_threshold_ohm = threshold;
					if (VDeviceSaveCfg != nullptr) {
						VDeviceSaveCfg();
					}
				}
			}
		} break;

		case 3: {
			/* Установка порога "Норма" (младший байт, старший байт) */
			if (Config != nullptr && Parameters != nullptr) {
				uint16_t threshold = Parameters[0] | (Parameters[1] << 8);
				if (threshold > 0) {
					Config->normal_threshold_ohm = threshold;
					normal_threshold_ohm = threshold;
					if (VDeviceSaveCfg != nullptr) {
						VDeviceSaveCfg();
					}
				}
			}
		} break;

		case 4: {
			/* Установка порога "Обрыв" (младший байт, старший байт) */
			if (Config != nullptr && Parameters != nullptr) {
				uint16_t threshold = Parameters[0] | (Parameters[1] << 8);
				if (threshold > 0) {
					Config->break_threshold_ohm = threshold;
					break_threshold_ohm = threshold;
					if (VDeviceSaveCfg != nullptr) {
						VDeviceSaveCfg();
					}
				}
			}
		} break;

		case 5: {
			/* Установка номинала резистора R1 (младший байт, старший байт) */
			if (Config != nullptr && Parameters != nullptr) {
				uint16_t r1 = Parameters[0] | (Parameters[1] << 8);
				if (r1 > 0) {
					Config->resistor_r1_ohm = r1;
					resistor_r1_ohm = r1;
					if (VDeviceSaveCfg != nullptr) {
						VDeviceSaveCfg();
					}
				}
			}
		} break;

		case 6: {
			/* Установка номинала резистора R2 (младший байт, старший байт) */
			if (Config != nullptr && Parameters != nullptr) {
				uint16_t r2 = Parameters[0] | (Parameters[1] << 8);
				if (r2 > 0) {
					Config->resistor_r2_ohm = r2;
					resistor_r2_ohm = r2;
					if (VDeviceSaveCfg != nullptr) {
						VDeviceSaveCfg();
					}
				}
			}
		} break;

		case 7: {
			/* Включение/выключение режима концевика
			 * Parameters[0] = 0 - обычный ДПТ (\"Пожар\")
			 * Parameters[0] = 1 - режим концевика (\"нажатие\")
			 */
			if (Config != nullptr && Parameters != nullptr) {
				uint8_t val = Parameters[0] ? 1 : 0;
				Config->is_limit_switch = val;
				is_limit_switch = val;
				if (VDeviceSaveCfg != nullptr) {
					VDeviceSaveCfg();
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

	/* Data[0] - состояние линии:
	 * 0 - Норма
	 * 1 - Обрыв
	 * 2 - КЗ
	 * 3 - Пожар
	 * 4 - Нажатие (режим концевика)
	 */
	Data[0] = LineState;

	/* Data[1..2] - измеренное сопротивление (младший байт, старший байт), Ом */
	Data[1] = measured_resistance_ohm & 0xFF;
	Data[2] = (measured_resistance_ohm >> 8) & 0xFF;
	Data[3] = (measured_resistance_ohm >> 16) & 0xFF;

	/* Data[4..5] - значения АЦП каналов (для отладки) */
	Data[4] = adc_ch1_value & 0xFF;
	Data[5] = (adc_ch1_value >> 8) & 0xFF;
	Data[6] = adc_ch2_value & 0xFF;

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
	Process();
}

void VDeviceDPT::SetAdcValues(uint16_t ch1, uint16_t ch2) {
	adc_ch1_value = ch1;
	adc_ch2_value = ch2;

	/* Рассчитываем сопротивление по первому каналу АЦП */
	measured_resistance_ohm = CalculateResistance(ch1);

	/* Обновляем состояние линии на основе измеренного сопротивления */
	UpdateLineState();
}

uint32_t VDeviceDPT::CalculateResistance(uint16_t adc_value) {
	/* Формула расчёта сопротивления линии по делителю напряжения:
	 * R_line = R2 * (V_supply / V_adc - 1) - R1
	 * где:
	 * V_adc = adc_value * supply_voltage_mv / adc_resolution
	 *
	 * Упрощённая формула:
	 * R_line = R2 * (adc_resolution / adc_value - 1) - R1
	 */

	if (adc_value == 0) {
		/* КЗ или ошибка измерения */
		return 0;
	}

	if (adc_value >= adc_resolution) {
		/* Обрыв - очень большое сопротивление */
		return break_threshold_ohm + 1000;
	}

	/* Расчёт сопротивления */
	uint32_t v_adc_mv = ((uint32_t)adc_value * supply_voltage_mv) / adc_resolution;

	if (v_adc_mv == 0) {
		return 0;
	}

	/* R_line = R2 * (V_supply / V_adc - 1) - R1 */
	uint32_t ratio = ((uint32_t)supply_voltage_mv * resistor_r2_ohm) / v_adc_mv;
	uint32_t r_line = ratio - resistor_r2_ohm - resistor_r1_ohm;

	return r_line;
}

void VDeviceDPT::UpdateLineState() {
	/* Определяем состояние линии на основе измеренного сопротивления */
	if (measured_resistance_ohm == 0) {
		LineState = DeviceDPTLineState_Short;  /* КЗ */
	} else if (measured_resistance_ohm <= fire_threshold_ohm) {
		/* При включенном режиме концевика считаем это \"нажатием\",
		 * иначе - событие \"Пожар\".
		 */
		if (is_limit_switch) {
			LineState = DeviceDPTLineState_Press;   /* Нажатие концевика */
		} else {
			LineState = DeviceDPTLineState_Fire;    /* Пожар (680 Ом) */
		}
	} else if (measured_resistance_ohm <= normal_threshold_ohm) {
		LineState = DeviceDPTLineState_Normal; /* Норма (5380 Ом) */
	} else if (measured_resistance_ohm > break_threshold_ohm) {
		LineState = DeviceDPTLineState_Break;  /* Обрыв */
	} else {
		/* Промежуточное значение - считаем нормой */
		LineState = DeviceDPTLineState_Normal;
	}

	if(prevLineState != LineState) {
		prevLineState = LineState;
	/* Обновляем статус при изменении состояния линии */
		SetStatus();
	}
}

