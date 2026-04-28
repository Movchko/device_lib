#include "device_lswitch.hpp"

#include <backend.h>

VDeviceLimitSwitch::VDeviceLimitSwitch(uint8_t ChNum)
	: VDeviceButton(ChNum),
	  LimitCfg(nullptr),
	  triggerDelayS(0u),
	  functionMode(DeviceLimitSwitchFunction_SetFault),
	  normalClosed(0u),
	  rawActive(0u),
	  confirmedActive(0u),
	  activeCounterMs(0u),
	  pauseSent(0u),
	  faultOutputArmed(0u) {
}

void VDeviceLimitSwitch::Init() {
	/* Используем базовую инициализацию DPT-логики, но свои действия на edge. */
	VDeviceDPT::Init();

	LimitCfg = nullptr;
	triggerDelayS = 0u;
	functionMode = DeviceLimitSwitchFunction_SetFault;
	normalClosed = 0u;
	rawActive = 0u;
	confirmedActive = 0u;
	activeCounterMs = 0u;
	pauseSent = 0u;
	faultOutputArmed = 0u;

	if (CfgPtr == nullptr) {
		return;
	}

	LimitCfg = reinterpret_cast<DeviceLimitSwitchConfig*>(CfgPtr->reserv);
	if (LimitCfg == nullptr) {
		return;
	}

	triggerDelayS = LimitCfg->trigger_delay_s;
	if (LimitCfg->function >= DeviceLimitSwitchFunction_SetFault &&
		LimitCfg->function <= DeviceLimitSwitchFunction_PauseStart) {
		functionMode = LimitCfg->function;
	}
	normalClosed = LimitCfg->normal_closed ? 1u : 0u;
}

void VDeviceLimitSwitch::OnPressEdge(void) {
	/* Для концевика действие на edge не требуется, только статус линии. */
}

DeviceDPTLineState VDeviceLimitSwitch::GetTriggeredLineState() const {
	/* Для функции "неисправность" Fault поднимаем только после trigger_delay_s. */
	if (functionMode == DeviceLimitSwitchFunction_SetFault && faultOutputArmed) {
		return DeviceDPTLineState_Fault;
	}
	return DeviceDPTLineState_Press;
}

uint8_t VDeviceLimitSwitch::IsRawTriggeredNow() const {
	DeviceDPTLineState line = GetLineState();
	uint8_t triggered = (line == DeviceDPTLineState_Press || line == DeviceDPTLineState_Fault) ? 1u : 0u;
	return normalClosed ? (triggered ? 0u : 1u) : triggered;
}

void VDeviceLimitSwitch::SendPpkuModeCommand(uint8_t mode) const {
	/* Нестандартная прикладная команда ППКУ в блоке CommandCB:
	 * cmd=13, param[0]=0(auto) / 1(manual). dir=0 обязателен, иначе non-service
	 * ветка ProtocolParse не вызовет CommandCB на получателе.
	 */
	can_ext_id_t can_id;
	can_id.ID = 0u;
	can_id.field.zone = 0u;
	can_id.field.h_adr = 0u;
	can_id.field.l_adr = 0u;
	can_id.field.d_type = DEVICE_PPKY_TYPE;
	can_id.field.dir = 1u;

	uint8_t data[8] = {0u};
	data[0] = 13u;
	data[1] = mode;
	SendMessageFull(can_id, data, SEND_NOW, BUS_CAN12);
}

void VDeviceLimitSwitch::HandleActiveTransition(uint8_t active_now) {
	if (active_now == confirmedActive) {
		return;
	}

	confirmedActive = active_now ? 1u : 0u;
	if (confirmedActive) {
		switch (functionMode) {
		case DeviceLimitSwitchFunction_SetFault:
			faultOutputArmed = 1u;
			break;

		case DeviceLimitSwitchFunction_SetManual:
			SendPpkuModeCommand(1u);
			break;

		case DeviceLimitSwitchFunction_SetAuto:
			SendPpkuModeCommand(0u);
			break;

		case DeviceLimitSwitchFunction_PauseStart:
			if (!pauseSent) {
				SetPauseExtinguishmentTimer(0u); /* broadcast */
				pauseSent = 1u;
			}
			break;

		default:
			break;
		}
	} else {
		faultOutputArmed = 0u;
		if (functionMode == DeviceLimitSwitchFunction_PauseStart && pauseSent) {
			SetResumeExtinguishmentTimer(0u); /* снимаем паузу при возврате концевика */
			pauseSent = 0u;
		}
	}
}

void VDeviceLimitSwitch::Timer1ms() {
	/* Базовая DPT-логика измерений/фильтрации/статусов */
	VDeviceDPT::Timer1ms();

	uint8_t raw_now = IsRawTriggeredNow();
	if (!raw_now) {
		rawActive = 0u;
		activeCounterMs = 0u;
		HandleActiveTransition(0u);
		return;
	}

	/* Активный уровень есть. */
	if (!rawActive) {
		rawActive = 1u;
		activeCounterMs = 0u;
	}

	uint32_t need_ms = (uint32_t)triggerDelayS * 1000u;
	if (activeCounterMs < need_ms) {
		activeCounterMs++;
	}

	if (activeCounterMs >= need_ms) {
		HandleActiveTransition(1u);
	}
}

void VDeviceLimitSwitch::CommandCB(uint8_t Command, uint8_t *Parameters) {
	/* Базовые команды DPT (12/13/14). */
	VDeviceDPT::CommandCB(Command, Parameters);

	if (LimitCfg == nullptr || Parameters == nullptr) {
		return;
	}

	switch (Command) {
	case 15: {
		/* trigger_delay_s */
		LimitCfg->trigger_delay_s = Parameters[0];
		triggerDelayS = Parameters[0];
		if (VDeviceSaveCfg != nullptr) {
			VDeviceSaveCfg();
		}
	} break;

	case 16: {
		/* function mode: 1..4 */
		if (Parameters[0] >= DeviceLimitSwitchFunction_SetFault &&
			Parameters[0] <= DeviceLimitSwitchFunction_PauseStart) {
			if (functionMode == DeviceLimitSwitchFunction_PauseStart && pauseSent &&
				Parameters[0] != DeviceLimitSwitchFunction_PauseStart) {
				SetResumeExtinguishmentTimer(0u);
				pauseSent = 0u;
			}
			LimitCfg->function = Parameters[0];
			functionMode = Parameters[0];
			faultOutputArmed = 0u;
			if (VDeviceSaveCfg != nullptr) {
				VDeviceSaveCfg();
			}
		}
	} break;

	case 17: {
		/* normal_closed: 0=NO, 1=NC */
		LimitCfg->normal_closed = Parameters[0] ? 1u : 0u;
		normalClosed = LimitCfg->normal_closed;
		if (VDeviceSaveCfg != nullptr) {
			VDeviceSaveCfg();
		}
	} break;

	default:
		break;
	}
}

uint8_t VDeviceLimitSwitch::GetDT() {
	return DEVICE_LSWITCH_TYPE;
}

