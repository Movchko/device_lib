
#ifndef INCLUDE_BACKEND_H_
#define INCLUDE_BACKEND_H_

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

	uint8_t 	reserv[UNIQ_ID_SIZE - 16]; // 32 - real data
} UniqId;

enum ServiceCmd {
	ServiceCmd_ResetMCU				= 128,
	ServiceCmd_StopStartSend		= 129,

};

enum ServiceConfigCmd {
	ServiceCmd_GetConfigSize 		= 150,
	ServiceCmd_GetConfigCRC    		= 151,
	ServiceCmd_GetConfigWord   		= 152,
	ServiceCmd_SetConfigWord	 	= 153,
	ServiceCmd_SaveConfig 			= 154,
	ServiceCmd_DefaultConfig 		= 155
};

void ServiceCommandParse(uint8_t Dev, uint8_t *MsgData);
void ProtocolParse(uint32_t ui32MsgID, uint8_t *pui8MsgData);

void BackendProcess(); // необходимо вызывать в главной программе. 1000герц

/* положить сообщение в очередь на отравку
 * now 1 - отправить без очереди
 */
void SendMessage(uint8_t Dev, uint8_t Cmd, uint8_t *Data, uint8_t Now);
void SendMessageFull(can_ext_id_t can_id, uint8_t *Data, uint8_t Now);

void ConfigServiceCmd(uint8_t Dev, uint8_t Command, uint8_t *MsgData);

void SetConfigPtr(uint8_t *SConfigPtr, uint8_t *LConfigPtr);


// описать в главной программе
void CANSendData(uint8_t *Buf);
void USBSendData(uint8_t *Buf);
void CommandCB(uint8_t Dev, uint8_t Command, uint8_t *Parameters);
void ListenerCommandCB(uint32_t MsgID, uint8_t *MsgData);
uint32_t GetID();
void FlashWriteData(uint8_t *ConfigPtr, uint16_t ConfigSize);
void ResetMCU();



// работа с конфигурацией
void DefaultConfig(); // restore default config
uint16_t GetConfigSize(); // get config size in  bytes
uint32_t GetConfigWord(uint16_t num); // get 4 bytes
void SetConfigWord(uint16_t num, uint32_t word); // set 4 bytes
void SaveConfig();

// выше функции описанныеы в главной программе
#endif /* INCLUDE_BACKEND_H_ */
