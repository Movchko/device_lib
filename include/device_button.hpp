#ifndef INCLUDE_DEVICE_BUTTON_HPP_
#define INCLUDE_DEVICE_BUTTON_HPP_

#include "device_dpt.hpp"
#include "device_config.h"

/* Hook для команды "ПУСК СП" (реализация ожидается в app конкретного проекта).
 * По умолчанию weak-реализация пустая. */
extern "C" void VDeviceButton_SendStartSP(void);

/* Локальный пуск спичек на МКУ (реализация в app): zone=0 — все зоны. */
extern "C" void VDeviceButton_OnStartExtinguishment(uint8_t zone,
                                                    uint8_t zone_delay_s,
                                                    uint8_t module_delay_s,
                                                    uint8_t launch_type);

class VDeviceButton : public VDeviceDPT {
protected:
	DeviceButtonConfig* ButtonCfg;
	DeviceButtonKind ButtonKind;
	uint8_t ZonesToStart[7];
	uint8_t normalClosed;
	uint8_t lastEffectivePressed;

	uint8_t IsEffectivePressed() const;
	DeviceDPTLineState GetTriggeredLineState() const override;
	virtual void OnPressEdge(void);

public:
	VDeviceButton(uint8_t ChNum);
	void Init() override;
	void Timer1ms() override;
	void CommandCB(uint8_t Command, uint8_t *Parameters) override;
	uint8_t GetDT() override;
};

#endif /* INCLUDE_DEVICE_BUTTON_HPP_ */

