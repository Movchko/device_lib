/*
 * service.h
 *
 *  Created on: Feb 16, 2026
 *      Author: 79099
 */

#ifndef SRC_SERVICE_H_
#define SRC_SERVICE_H_
#include <stdint.h>
uint32_t crc32(uint32_t crc, const void *buf, uint32_t size);
uint16_t CRC16(uint8_t *Buf);
#define POLYNOM     ((unsigned short)0x8005)

#endif /* SRC_SERVICE_H_ */
