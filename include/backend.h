
#ifndef INCLUDE_BACKEND_H_
#define INCLUDE_BACKEND_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>


#define CDCPRE1    0xC4
#define CDCPRE2    0x5B
#define CDCPKTLEN  (2 + 12 + 2)
extern uint8_t USBSndBuf[CDCPKTLEN];

#define MAX_DEVS   16
#define SEND_DELAY_MS 5 // задержка между посылками внутри одного устройства. т.е физически не может одно устйроство слать больше 1000 пакетов в секунду
#define NumSendMsgObj 100 // буфер отсылаемых пакетов
#define SEND_NOW	1

#define UNIQ_ID_SIZE	32

#define BUS_CAN0		1
#define BUS_CAN1		2
#define BUS_CAN12		3

typedef union {
  uint32_t ID; /* Идентификатор в форме 32-х разрядного числа */
  struct {
    uint32_t  zone:7;       /* Номер зоны */
    uint32_t  l_adr:6; 		/* Младшая часть адреса */
    uint32_t  h_adr:8;      /* Старшая часть адреса */
    uint32_t  d_type:7;     /* Тип устройства */
    uint32_t  dir:1;   		/* Направление */
  } field;
} can_ext_id_t;

typedef struct {
    uint8_t  zone;       /* Номер зоны */
    uint8_t  l_adr; 	 /* Младшая часть адреса */
    uint8_t  h_adr;      /* Старшая часть адреса */
    uint8_t  d_type;     /* Тип устройства */
} Device;

typedef struct {
	uint32_t 	UId0;
	uint32_t 	UId1;
	uint32_t 	UId2;
	uint32_t 	UId3;
	uint32_t	UId4;
	Device 		devId;

	uint8_t 	reserv[UNIQ_ID_SIZE - 24]; // 32 - real data
} UniqId;

enum ServiceCmd {
	ServiceCmd_ResetMCU				= 128,
	ServiceCmd_StopStartSend		= 129,
	ServiceCmd_StopStartReTranslate	= 130,

	ServiceCmd_SetStatusFire		= 140,
	ServiceCmd_ReplyStatusFire		= 141,
	ServiceCmd_StartExtinguishment 	= 142,
	ServiceCmd_StopExtinguishment 	= 143,

	ServiceCmd_GetConfigSize 		= 150,
	ServiceCmd_GetConfigCRC    		= 151,
	ServiceCmd_GetConfigWord   		= 152,
	ServiceCmd_SetConfigWord	 	= 153,
	ServiceCmd_SaveConfig 			= 154,
	ServiceCmd_DefaultConfig 		= 155,

	ServiceCmd_SetSystemTime		= 157,

	ServiceCmd_CircSetAdr 		= 200,
};


// bus - битовая маска - номер шины (0b01 - CAN 0, 0b10 - CAN 1)
void ServiceCommandParse(uint8_t Dev, uint8_t Command, uint8_t *MsgData, uint8_t bus, uint8_t dir);
void ProtocolParse(uint32_t ui32MsgID, uint8_t *pui8MsgData, uint8_t bus);

void BackendProcess(); // необходимо вызывать в главной программе. 1000герц


/* положить сообщение в очередь на отравку
 * now 1 - отправить без очереди
 */
void SendMessage(uint8_t Dev, uint8_t Cmd, uint8_t *Data, uint8_t Now, uint8_t bus);
void SendMessageFull(can_ext_id_t can_id, uint8_t *Data, uint8_t Now, uint8_t bus);
void SendAllMessage(uint8_t Cmd, uint8_t *Data, uint8_t Now, uint8_t bus);

void SetConfigPtr(uint8_t *SConfigPtr, uint8_t *LConfigPtr);

uint8_t GetRetranslate(); // вернуть флаг разрешена ли ретрансляция сообщений

void SetStatusFire();
void SetReplyStatusFire();
void SetStartExtinguishment(uint8_t zone);
void SetStopExtinguishment();

void RcvStatusFire();
void RcvReplyStatusFire();
void RcvStartExtinguishment();
void RcvStopExtinguishment();

// описать в главной программе
void CANSendData(uint8_t *Buf);
void USBSendData(uint8_t *Buf);

/* Вызывается при переполнении очереди отправки; можно переопределить в приложении */
void CanSendOverError(void);
/* Количество отброшенных сообщений из-за переполнения (сброс — в приложении при необходимости) */
uint32_t BackendGetSendOverflowCount(void);
/* Количество подключённых устройств МКУ */
uint8_t BackendGetDeviceCount(void);
void CommandCB(uint8_t Dev, uint8_t Command, uint8_t *Parameters);
void ListenerCommandCB(uint32_t MsgID, uint8_t *MsgData);
uint32_t GetID();
void FlashWriteData(uint8_t *ConfigPtr, uint32_t ConfigSize);
void ResetMCU();
void SetHAdr(uint8_t h_adr);
void RcvSetSystemTime(uint8_t *MsgData);


// работа с конфигурацией
void DefaultConfig(); // restore default config
uint32_t GetConfigSize(); // get config size in  bytes
uint32_t GetConfigWord(uint16_t num); // get 4 bytes
void SetConfigWord(uint16_t num, uint32_t word); // set 4 bytes
void SaveConfig();
// выше функции описанныеы в главной программе



/***********************************************************************************************************
 * Оболочка пакета для посылок по другим интерфейсам (USB, UART и т.д.)
 *
 * Формат пакета:
 *   2 байта - преамбула (0xAA55, little-endian: 0x55, 0xAA)
 *   2 байта - размер всего пакета (little-endian)
 *   2 байта - тип пакета (little-endian, 0 = CAN, будут и другие)
 *   2 байта - номер пакета (little-endian, для CAN всегда 0)
 *   ... полезные данные ...
 *   2 байта - контрольная сумма (16-bit sum по всем байтам кроме последних 2)
 *
 * Тип 0 = CAN пакет: 4 байта ID (little-endian) + 8 байт данных
 ***********************************************************************************************************/

#define BSU_PKT_PREAMBLE     0xAA55u
#define BSU_PKT_PREAMBLE_LO  0x55u
#define BSU_PKT_PREAMBLE_HI  0xAAu

#define BSU_PKT_TYPE_CAN     0u
#define BSU_PKT_TYPE_CAN2    1u

#define BSU_PKT_HEADER_SIZE  (2u + 2u + 2u + 2u)  /* preamble + size + type + seq */
#define BSU_PKT_CAN_PAYLOAD  (4u + 8u)           /* id + data */
#define BSU_PKT_CHECKSUM_SIZE 2u
#define BSU_PKT_CAN_SIZE     (BSU_PKT_HEADER_SIZE + BSU_PKT_CAN_PAYLOAD + BSU_PKT_CHECKSUM_SIZE)

/** Контрольная сумма: 16-bit sum по data[0..len-1] */
uint16_t BSU_Checksum(const uint8_t *data, uint32_t len);

/**
 * Формирование CAN-пакета для отправки.
 * @param out_buf   буфер для записи пакета (минимум BSU_PKT_CAN_SIZE байт)
 * @param buf_size  размер out_buf
 * @param can_id    29-bit CAN ID (little-endian в пакете)
 * @param data      8 байт данных
 * @return длина сформированного пакета (BSU_PKT_CAN_SIZE) или 0 при ошибке
 */
uint16_t BSU_PacketBuildCan(uint8_t *out_buf, uint32_t buf_size, uint32_t can_id, const uint8_t *data);

/**
 * Парсинг принятого пакета.
 * @param buf       буфер с полным пакетом
 * @param len       длина buf (должна быть >= BSU_PKT_CAN_SIZE для типа CAN)
 * @param out_can_id  [out] извлечённый CAN ID (для типа CAN)
 * @param out_data   [out] извлечённые 8 байт данных (для типа CAN)
 * @return 1 при успешном разборе CAN-пакета, 0 при ошибке (неверный формат, checksum, тип)
 */
uint8_t BSU_PacketParse(const uint8_t *buf, uint32_t len, uint32_t *out_can_id, uint8_t *out_data);

#ifdef __cplusplus
}
#endif


#endif /* INCLUDE_BACKEND_H_ */
