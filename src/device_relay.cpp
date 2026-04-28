#include <device_relay.hpp>

VDeviceRelay::VDeviceRelay(uint8_t ChNum) : VDevice(ChNum)
{
	Config = nullptr;
	Status = DeviceRelayStatus_Idle;
	Counter1s = 0;
	desired_state = 0;
	actual_state = 0;
	error_flag = 0;
	settle_time_ms = 100;
	settle_counter_ms = 0;
	feedback_inverted = 0;
	persist_state_enabled = 0;
	switch_delay_s = 0u;
	pending_switch = 0u;
	pending_state = 0u;
	switch_delay_counter_ms = 0u;
}

void VDeviceRelay::Init()
{
	if (CfgPtr != nullptr) {
		Config = reinterpret_cast<DeviceRelayConfig*>(CfgPtr->reserv);
	} else {
		Config = nullptr;
	}

	if (Config != nullptr) {
		desired_state = (Config->initial_state != 0u) ? 1u : 0u;
		persist_state_enabled = (Config->persist_state_enabled != 0u) ? 1u : 0u;
		feedback_inverted = (Config->feedback_inverted != 0u) ? 1u : 0u;
		switch_delay_s = Config->switch_delay_s;
		settle_time_ms = (Config->settle_time_ms != 0u) ? Config->settle_time_ms : 100u;
	} else {
		desired_state = 0u;
		persist_state_enabled = 0u;
		feedback_inverted = 0u;
		switch_delay_s = 0u;
		settle_time_ms = 100u;
	}

	error_flag = 0u;
	settle_counter_ms = 0u;
	pending_switch = 0u;
	pending_state = desired_state;
	switch_delay_counter_ms = 0u;
	ApplyOutput(desired_state);
	actual_state = ReadFeedbackState();
	UpdateStatus(DeviceRelayStatus_Idle);
}

void VDeviceRelay::ApplyOutput(uint8_t state)
{
	if (Relay_SetOutput != nullptr) {
		Relay_SetOutput(state ? 1u : 0u);
	}
}

uint8_t VDeviceRelay::ReadFeedbackState(void) const
{
	uint8_t fb = 0u;
	if (Relay_GetFeedback != nullptr) {
		fb = Relay_GetFeedback() ? 1u : 0u;
	}
	if (feedback_inverted != 0u) {
		fb = (fb == 0u) ? 1u : 0u;
	}
	return fb;
}

void VDeviceRelay::Process()
{
	if (pending_switch != 0u) {
		if (switch_delay_counter_ms > 0u) {
			switch_delay_counter_ms--;
		}
		if (switch_delay_counter_ms == 0u) {
			desired_state = pending_state;
			ApplyOutput(desired_state);
			settle_counter_ms = 0u;
			pending_switch = 0u;
			SavePersistentStateIfNeeded();
		}
	}

	actual_state = ReadFeedbackState();

	if (settle_counter_ms < settle_time_ms) {
		settle_counter_ms++;
		return;
	}

	if (actual_state != desired_state) {
		if(error_flag == 0) {
			error_flag = 1u;
			UpdateStatus(DeviceRelayStatus_Error);
		}
	} else if(error_flag) {
		error_flag = 0u;
		UpdateStatus(DeviceRelayStatus_Idle);

	}
}

void VDeviceRelay::SavePersistentStateIfNeeded(void)
{
	if (Config == nullptr || persist_state_enabled == 0u) {
		return;
	}
	Config->initial_state = desired_state;
	if (VDeviceSaveCfg != nullptr) {
		VDeviceSaveCfg();
	}
}

void VDeviceRelay::CommandCB(uint8_t Command, uint8_t *Parameters)
{
	if (Command == 10u) {
		uint8_t new_state = desired_state;

		/* cmd=10: переключение реле.
		 * Если Parameters[0] == 0/1 — установить явно.
		 * Иначе (или без параметра) — инвертировать текущее желаемое состояние. */
		if (Parameters != nullptr && (Parameters[0] == 0u || Parameters[0] == 1u)) {
			new_state = Parameters[0];
		} else {
			new_state = (desired_state == 0u) ? 1u : 0u;
		}

		if (switch_delay_s > 0u) {
			pending_state = new_state;
			pending_switch = 1u;
			switch_delay_counter_ms = (uint32_t)switch_delay_s * 1000u;
			if (switch_delay_counter_ms == 0u) {
				switch_delay_counter_ms = 1u;
			}
		} else {
			desired_state = new_state;
			ApplyOutput(desired_state);
			settle_counter_ms = 0u;
			pending_switch = 0u;
			SavePersistentStateIfNeeded();
		}
	}

}

void VDeviceRelay::UpdateStatus(DeviceRelayStatus status)
{
	Status = status;
	SetStatus();
}

void VDeviceRelay::SetStatus()
{
	uint8_t Data[7] = {0, 0, 0, 0, 0, 0, 0};
	/* Data[0] = actual_state (0/1)
	 * Data[1] = error_flag  (0/1)
	 * Data[2] = desired_state (0/1) для диагностики */
	Data[0] = actual_state;
	Data[1] = error_flag;
	Data[2] = desired_state;

	if (VDeviceSetStatus != nullptr) {
		VDeviceSetStatus(Num, Status, Data);
	}
}

void VDeviceRelay::Timer1ms()
{
	Counter1s++;
	if (Counter1s >= 1000u) {
		SetStatus();
		Counter1s = 0u;
	}
	Process();
}

