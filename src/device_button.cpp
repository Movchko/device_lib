#include "device_button.hpp"

#include <backend.h>
#include <string.h>

extern "C" __attribute__((weak)) void VDeviceButton_SendStartSP(void) {
}

VDeviceButton::VDeviceButton(uint8_t ChNum)
	: VDeviceDPT(ChNum), ButtonCfg(nullptr), ButtonKind(DeviceButtonKind_StartSP), LastLineState(DeviceDPTLineState_Normal) {
	memset(ZonesToStart, 0, sizeof(ZonesToStart));
}

DeviceDPTLineState VDeviceButton::GetTriggeredLineState() const {
	return DeviceDPTLineState_Press;
}

void VDeviceButton::Init() {
	VDeviceDPT::Init();

	ButtonCfg = nullptr;
	ButtonKind = DeviceButtonKind_StartSP;
	memset(ZonesToStart, 0, sizeof(ZonesToStart));
	LastLineState = GetLineState();

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

	memcpy(ZonesToStart, ButtonCfg->zones, sizeof(ZonesToStart));
}

void VDeviceButton::OnPressEdge(void) {
	uint8_t data[7] = {0, 0, 0, 0, 0, 0, 0};

	switch (ButtonKind) {
	case DeviceButtonKind_StartSP:
		VDeviceButton_SendStartSP();
		break;

	case DeviceButtonKind_StartAll:
		/* StartAll: data[0]=1 (тип), зона=0 (broadcast). */
		data[0] = 1u;
		SendAllMessage(ServiceCmd_StartExtinguishment, data, SEND_NOW, BUS_CAN12);
		break;

	case DeviceButtonKind_StartZone:
		/* Запуск каждой зоны из конфигурации (до 7 зон). */
		for (uint8_t i = 0; i < sizeof(ZonesToStart); i++) {
			uint8_t zone = ZonesToStart[i];
			if (zone == 0u) {
				continue;
			}
			data[0] = zone;
			SendAllMessage(ServiceCmd_StartExtinguishment, data, SEND_NOW, BUS_CAN12);
		}
		break;
	}
}

void VDeviceButton::Timer1ms() {
	VDeviceDPT::Timer1ms();

	DeviceDPTLineState current = GetLineState();
	if ((LastLineState != DeviceDPTLineState_Press) && (current == DeviceDPTLineState_Press)) {
		OnPressEdge();
	}
	LastLineState = current;
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

	default:
		break;
	}
}

uint8_t VDeviceButton::GetDT() {
	return DEVICE_BUTTON_TYPE;
}

