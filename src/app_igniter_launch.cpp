#include "app_igniter_launch.hpp"

void AppIgniter_RunSequentialScheduler(uint8_t slot_count,
                                       uint32_t now_ms,
                                       uint32_t *deadline_ms,
                                       const uint8_t *armed,
                                       const uint8_t *paused,
                                       AppIgniterSlotPredFn is_igniter_slot,
                                       AppIgniterSlotPredFn is_burn_running,
                                       AppIgniterFireFn fire,
                                       void *ctx)
{
	if (deadline_ms == nullptr || armed == nullptr ||
	    is_igniter_slot == nullptr || is_burn_running == nullptr || fire == nullptr) {
		return;
	}

	for (uint8_t i = 0u; i < slot_count; i++) {
		if (!armed[i]) {
			continue;
		}
		if ((paused != nullptr) && paused[i]) {
			continue;
		}
		if (!is_igniter_slot(i, ctx)) {
			continue;
		}
		if ((int32_t)(now_ms - deadline_ms[i]) < 0) {
			continue;
		}

		uint8_t blocked = 0u;
		for (uint8_t j = 0u; j < slot_count; j++) {
			if (j == i) {
				continue;
			}
			if (!is_igniter_slot(j, ctx)) {
				continue;
			}
			if (is_burn_running(j, ctx)) {
				blocked = 1u;
				break;
			}
		}
		if (blocked) {
			continue;
		}

		fire(i, ctx);
		return;
	}
}
