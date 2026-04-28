#ifndef INCLUDE_DEVICE_LSWITCH_HPP_
#define INCLUDE_DEVICE_LSWITCH_HPP_

#include "device_button.hpp"

class VDeviceLimitSwitch : public VDeviceButton {
	DeviceLimitSwitchConfig* LimitCfg;
	uint8_t triggerDelayS;
	uint8_t functionMode;
	uint8_t normalClosed;
	uint8_t rawActive;
	uint8_t confirmedActive;
	uint32_t activeCounterMs;
	uint8_t pauseSent;
	uint8_t faultOutputArmed;

	uint8_t IsRawTriggeredNow() const;
	void HandleActiveTransition(uint8_t active_now);
	void SendPpkuModeCommand(uint8_t mode) const;

protected:
	DeviceDPTLineState GetTriggeredLineState() const override;
	void OnPressEdge(void) override;

public:
	VDeviceLimitSwitch(uint8_t ChNum);
	void Init() override;
	void Timer1ms() override;
	void CommandCB(uint8_t Command, uint8_t *Parameters) override;
	uint8_t GetDT() override;
};

#endif /* INCLUDE_DEVICE_LSWITCH_HPP_ */

