#include "device_lswitch.hpp"

VDeviceLimitSwitch::VDeviceLimitSwitch(uint8_t ChNum)
	: VDeviceButton(ChNum) {
}

void VDeviceLimitSwitch::OnPressEdge(void) {
	/* Для концевика действие на edge не требуется, только статус линии. */
}

uint8_t VDeviceLimitSwitch::GetDT() {
	return DEVICE_LSWITCH_TYPE;
}

