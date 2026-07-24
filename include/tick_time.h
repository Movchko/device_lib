#ifndef TICK_TIME_H
#define TICK_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Сравнения HAL_GetTick() / now_ms.
 *
 * Классический uint32 now-then при then>now (IRQ обновил метку после снимка now)
 * даёт underflow ~0xFFFFFFFF и ложный «таймаут». Знаковый delta это чинит:
 * отрицательный возраст = «событие только что / в будущем относительно now» → не expired.
 * Для интервалов < ~24 суток также корректно переживает wrap uwTick.
 */

static inline int32_t TickDeltaMs(uint32_t now_ms, uint32_t then_ms)
{
	return (int32_t)(now_ms - then_ms);
}

/* 1 = возраст then_ms ещё в пределах timeout (в т.ч. then_ms чуть новее now). */
static inline uint8_t TickAgeWithinMs(uint32_t now_ms, uint32_t then_ms, uint32_t timeout_ms)
{
	return (TickDeltaMs(now_ms, then_ms) <= (int32_t)timeout_ms) ? 1u : 0u;
}

/* 1 = возраст then_ms >= timeout (если then_ms новее now — 0). */
static inline uint8_t TickAgeExpiredMs(uint32_t now_ms, uint32_t then_ms, uint32_t timeout_ms)
{
	return (TickDeltaMs(now_ms, then_ms) >= (int32_t)timeout_ms) ? 1u : 0u;
}

#ifdef __cplusplus
}
#endif

#endif /* TICK_TIME_H */
