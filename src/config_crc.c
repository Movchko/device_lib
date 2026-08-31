#include "config_crc.h"

#include "backend.h"
#include "device_cfg_common.h"
#include "device_config.h"
#include "service.h"

#include <stddef.h>
#include <string.h>

static uint8_t ConfigCrc_IsExcluded(uint32_t pos, uint32_t len,
				    const ConfigCrcExclude *excludes, uint8_t exclude_count)
{
	for (uint8_t i = 0u; i < exclude_count; i++) {
		uint32_t start = excludes[i].offset;
		uint32_t end = start + excludes[i].size;
		if (pos < end && (pos + len) > start) {
			return 1u;
		}
	}
	return 0u;
}

uint32_t ConfigCrc32Excluding(uint32_t crc, const void *blob, uint32_t blob_size,
			      const ConfigCrcExclude *excludes, uint8_t exclude_count)
{
	const uint8_t *p = (const uint8_t *)blob;
	uint32_t pos = 0u;

	if (blob == NULL || blob_size == 0u) {
		return crc;
	}

	while (pos < blob_size) {
		uint32_t chunk = blob_size - pos;
		uint32_t next_ex = blob_size;

		for (uint8_t i = 0u; i < exclude_count; i++) {
			uint32_t ex_start = excludes[i].offset;
			uint32_t ex_end = ex_start + excludes[i].size;
			if (ex_start <= pos && pos < ex_end) {
				pos = ex_end;
				chunk = 0u;
				break;
			}
			if (ex_start > pos && ex_start < next_ex) {
				next_ex = ex_start;
			}
		}

		if (chunk == 0u) {
			continue;
		}
		if (next_ex < pos + chunk) {
			chunk = next_ex - pos;
		}
		if (ConfigCrc_IsExcluded(pos, chunk, excludes, exclude_count) != 0u) {
			pos += chunk;
			continue;
		}
		crc = crc32(crc, p + pos, chunk);
		pos += chunk;
	}

	return crc;
}

uint8_t MkuCfg_BuildCrcExcludes(const void *mkucfg, ConfigCrcExclude *out, uint8_t max_out)
{
	const MKUCfg *cfg = (const MKUCfg *)mkucfg;
	uint8_t n = 0u;

	if (cfg == NULL || out == NULL || max_out == 0u) {
		return 0u;
	}

	for (uint8_t i = 0u; i < NUM_DEV_IN_MCU; i++) {
		if ((cfg->VDtype[i] & 0xFFu) != DEVICE_RELAY_TYPE) {
			continue;
		}
		const DeviceRelayConfig *relay =
			(const DeviceRelayConfig *)&cfg->Devices[i].reserv;
		if (relay->persist_state_enabled == 0u) {
			continue;
		}
		if (n >= max_out) {
			break;
		}
		out[n].offset = (uint32_t)(offsetof(MKUCfg, Devices[i].reserv) +
					   offsetof(DeviceRelayConfig, saved_state));
		out[n].size = (uint32_t)sizeof(relay->saved_state);
		n++;
	}

	return n;
}

uint32_t MkuCfg_ComputeCrc(const void *mkucfg)
{
	ConfigCrcExclude excludes[CONFIG_CRC_EXCLUDE_MAX];
	uint8_t n = MkuCfg_BuildCrcExcludes(mkucfg, excludes, CONFIG_CRC_EXCLUDE_MAX);

	return ConfigCrc32Excluding(POLYNOM, mkucfg, (uint32_t)sizeof(MKUCfg), excludes, n);
}
