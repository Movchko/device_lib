#ifndef INCLUDE_APP_IGNITER_LAUNCH_HPP_
#define INCLUDE_APP_IGNITER_LAUNCH_HPP_

#include <stdint.h>

typedef uint8_t (*AppIgniterSlotPredFn)(uint8_t slot, void *ctx);
typedef void (*AppIgniterFireFn)(uint8_t slot, void *ctx);

/* Пуск не более одной спички за тик; следующая — только после Run→Idle у другой. */
void AppIgniter_RunSequentialScheduler(uint8_t slot_count,
                                       uint32_t now_ms,
                                       uint32_t *deadline_ms,
                                       const uint8_t *armed,
                                       const uint8_t *paused,
                                       AppIgniterSlotPredFn is_igniter_slot,
                                       AppIgniterSlotPredFn is_burn_running,
                                       AppIgniterFireFn fire,
                                       void *ctx);

#endif /* INCLUDE_APP_IGNITER_LAUNCH_HPP_ */
