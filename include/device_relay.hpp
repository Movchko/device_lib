#ifndef INCLUDE_DEVICE_RELAY_HPP_
#define INCLUDE_DEVICE_RELAY_HPP_

#include "device.hpp"
#include "device_config.h"

enum DeviceRelayStatus {
	DeviceRelayStatus_Idle = 0,
	DeviceRelayStatus_Error = 1
};

class VDeviceRelay: public VDevice {
	DeviceRelayConfig *Config;
	DeviceRelayStatus Status;
	uint32_t Counter1s;

	uint8_t desired_state;
	uint8_t actual_state;
	uint8_t error_flag;
	uint16_t settle_time_ms;
	uint16_t settle_counter_ms;
	uint8_t feedback_inverted;
	uint8_t persist_state_enabled;
	uint8_t switch_delay_s;
	uint8_t pending_switch;
	uint8_t pending_state;
	uint32_t switch_delay_counter_ms;

	void UpdateStatus(DeviceRelayStatus status);
	void ApplyOutput(uint8_t state);
	uint8_t ReadFeedbackState(void) const;
	void SavePersistentStateIfNeeded(void);

public:
	VDeviceRelay(uint8_t ChNum);

	uint8_t GetDT() { return DEVICE_RELAY_TYPE; }
	void Init();
	void Process();
	void CommandCB(uint8_t Command, uint8_t *Parameters);
	void SetStatus();
	void Timer1ms();

	/* Колбеки от app:
	 * Relay_SetOutput(state): управление GPIO выхода реле
	 * Relay_GetFeedback(): чтение GPIO обратной связи (0/1) */
	void (*Relay_SetOutput)(uint8_t state) = nullptr;
	uint8_t (*Relay_GetFeedback)(void) = nullptr;
};

#endif /* INCLUDE_DEVICE_RELAY_HPP_ */

