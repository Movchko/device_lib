#include "device_button.hpp"

#include <backend.h>
#include <string.h>

extern "C" __attribute__((weak)) void VDeviceButton_SendStartSP(void) {
}

extern "C" __attribute__((weak)) void VDeviceButton_OnStartExtinguishment(uint8_t zone,
                                                                        uint8_t zone_delay_s,
                                                                        uint8_t module_delay_s,
                                                                        uint8_t launch_type) {
	(void)zone;
	(void)zone_delay_s;
	(void)module_delay_s;
	(void)launch_type;
}

static void VDeviceButton_SendStartExtinguishment(uint8_t zone,
                                                uint8_t zone_delay_s,
                                                uint8_t module_delay_s,
                                                uint8_t launch_type)
{
	can_ext_id_t can_id;
	uint8_t data[8] = {
		ServiceCmd_Fire_StartExtinguishment,
		zone,
		zone_delay_s,
		module_delay_s,
		launch_type,
		0u, 0u, 0u
	};

	can_id.ID = 0u;
	can_id.field.dir = 0u;
	can_id.field.d_type = 0u;
	can_id.field.h_adr = 0u;
	can_id.field.l_adr = 0u;
	can_id.field.zone = zone & 0x7Fu;

	SendMessageFull(can_id, data, SEND_NOW, BUS_CAN12);
	VDeviceButton_OnStartExtinguishment(zone, zone_delay_s, module_delay_s, launch_type);
}

static void VDeviceButton_SendStartSpButton(void)
{
	can_ext_id_t can_id;
	uint8_t data[8] = {
		ServiceCmd_Fire_StartSpButton,
		0u, 0u, 0u, 0u, 0u, 0u, 0u
	};

	can_id.ID = 0u;
	can_id.field.dir = 0u;
	can_id.field.d_type = 0u;
	can_id.field.h_adr = 0u;
	can_id.field.l_adr = 0u;
	can_id.field.zone = 0u;

	SendMessageFull(can_id, data, SEND_NOW, BUS_CAN12);
}

VDeviceButton::VDeviceButton(uint8_t ChNum)
	: VDeviceDPT(ChNum),
	  ButtonCfg(nullptr),
	  ButtonKind(DeviceButtonKind_StartSP),
	  normalClosed(0u),
	  lastEffectivePressed(0u) {
	memset(ZonesToStart, 0, sizeof(ZonesToStart));
}

uint8_t VDeviceButton::IsEffectivePressed() const {
	/* NC/NO учтён в app (App_MapLswitchResistanceForLib). */
	return (GetLineState() == DeviceDPTLineState_Press) ? 1u : 0u;
}

DeviceDPTLineState VDeviceButton::GetTriggeredLineState() const {
	return DeviceDPTLineState_Press;
}

void VDeviceButton::Init() {
	VDeviceDPT::Init();

	ButtonCfg = nullptr;
	ButtonKind = DeviceButtonKind_StartSP;
	normalClosed = 0u;
	lastEffectivePressed = 0u;
	memset(ZonesToStart, 0, sizeof(ZonesToStart));

	if (CfgPtr == nullptr) {
		return;
	}

	ButtonCfg = reinterpret_cast<DeviceButtonConfig*>(CfgPtr->reserv);
	if (ButtonCfg == nullptr) {
		return;
	}

	if (ButtonCfg->button_kind <= DeviceButtonKind_StartZone) {
		ButtonKind = static_cast<DeviceButtonKind>(ButtonCfg->button_kind);
	}
	normalClosed = ButtonCfg->normal_closed ? 1u : 0u;

	memcpy(ZonesToStart, ButtonCfg->zones, sizeof(ZonesToStart));
	lastEffectivePressed = IsEffectivePressed();
}

void VDeviceButton::OnPressEdge(void) {
	switch (ButtonKind) {
	case DeviceButtonKind_StartSP:
		/* ПУСК СП на ППКУ: broadcast ServiceCmd_Fire_StartSpButton (163). */
		VDeviceButton_SendStartSpButton();
		break;

	case DeviceButtonKind_StartAll:
		/* Пуск всех зон: внутренняя задержка модуля (как ПУСК ОБЩИЙ на ППКУ). */
		VDeviceButton_SendStartExtinguishment(0u, 0u, 0u, START_EXT_DELAY_MODULE_ONLY);
		break;

	case DeviceButtonKind_StartZone:
		for (uint8_t i = 0; i < sizeof(ZonesToStart); i++) {
			uint8_t zone = ZonesToStart[i];
			if (zone == 0u) {
				continue;
			}
			VDeviceButton_SendStartExtinguishment(zone, 0u, 0u, START_EXT_DELAY_MODULE_ONLY);
		}
		break;
	}
}

void VDeviceButton::Timer1ms() {
	VDeviceDPT::Timer1ms();

	uint8_t effective_now = IsEffectivePressed();
	if (!lastEffectivePressed && effective_now) {
		OnPressEdge();
	}
	lastEffectivePressed = effective_now;
}

void VDeviceButton::CommandCB(uint8_t Command, uint8_t *Parameters) {
	/* Команды DPT: порог/фильтр/mode(legacy). */
	VDeviceDPT::CommandCB(Command, Parameters);

	switch (Command) {
	case 15: {
		/* Вид кнопки: 0=StartSP, 1=StartAll, 2=StartZone */
		if ((ButtonCfg != nullptr) && (Parameters != nullptr) && (Parameters[0] <= DeviceButtonKind_StartZone)) {
			ButtonCfg->button_kind = Parameters[0];
			ButtonKind = static_cast<DeviceButtonKind>(Parameters[0]);
			if (VDeviceSaveCfg != nullptr) {
				VDeviceSaveCfg();
			}
		}
	} break;

	case 16: {
		/* Список зон (7 байт) для режима StartZone. */
		if ((ButtonCfg != nullptr) && (Parameters != nullptr)) {
			memcpy(ButtonCfg->zones, Parameters, sizeof(ZonesToStart));
			memcpy(ZonesToStart, Parameters, sizeof(ZonesToStart));
			if (VDeviceSaveCfg != nullptr) {
				VDeviceSaveCfg();
			}
		}
	} break;

	case 17: {
		/* normal_closed: 0=NO, 1=NC */
		if ((ButtonCfg != nullptr) && (Parameters != nullptr)) {
			ButtonCfg->normal_closed = Parameters[0] ? 1u : 0u;
			normalClosed = ButtonCfg->normal_closed;
			lastEffectivePressed = IsEffectivePressed();
			if (VDeviceSaveCfg != nullptr) {
				VDeviceSaveCfg();
			}
		}
	} break;

	default:
		break;
	}
}

uint8_t VDeviceButton::GetDT() {
	return DEVICE_BUTTON_TYPE;
}

