#ifndef INCLUDE_DEVICE_LSWITCH_HPP_
#define INCLUDE_DEVICE_LSWITCH_HPP_

#include "device_button.hpp"

/* Концевик переиспользует логику виртуальной кнопки (фильтрация и статус Press),
 * но не выполняет действий при нажатии. */
class VDeviceLimitSwitch : public VDeviceButton {
protected:
	void OnPressEdge(void) override;

public:
	VDeviceLimitSwitch(uint8_t ChNum);
	uint8_t GetDT() override;
};

#endif /* INCLUDE_DEVICE_LSWITCH_HPP_ */

