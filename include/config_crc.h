#ifndef INCLUDE_CONFIG_CRC_H_
#define INCLUDE_CONFIG_CRC_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Исключение volatile-поля из CRC образа конфигурации.
 * Механизм универсален: таблица байтовых диапазонов, не участвующих в сверке.
 */
typedef struct ConfigCrcExclude {
	uint32_t offset;
	uint32_t size;
} ConfigCrcExclude;

#define CONFIG_CRC_EXCLUDE_MAX 64u

/** CRC32 с пропуском указанных диапазонов (crc/init — как crc32() в service.h). */
uint32_t ConfigCrc32Excluding(uint32_t crc, const void *blob, uint32_t blob_size,
			      const ConfigCrcExclude *excludes, uint8_t exclude_count);

/** Собрать исключения для MKUCfg (saved_state реле с persist_state_enabled). */
uint8_t MkuCfg_BuildCrcExcludes(const void *mkucfg, ConfigCrcExclude *out, uint8_t max_out);

/** CRC MKUCfg с учётом volatile-полей реле. */
uint32_t MkuCfg_ComputeCrc(const void *mkucfg);

#ifdef __cplusplus
}
#endif

#endif /* INCLUDE_CONFIG_CRC_H_ */
