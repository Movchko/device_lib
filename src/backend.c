
#include "backend.h"
#include "service.h"

uint8_t isMaster = 0;

uint8_t nDevs = 0;
Device BoardDevicesList[MAX_DEVS];

Device ListenerDevicesList[MAX_DEVS];
bool isListener = false;
uint8_t ListenerDevNum = 0;

bool isCanActive = false;
uint8_t send_delay = 0;
typedef struct {
    uint32_t ui32MsgID;
    uint8_t pui8MsgData[8];
    uint8_t bus;
}
tBufMsgObject;
tBufMsgObject BufSendMsgObj[NumSendMsgObj];
uint8_t       IndexSendMsgObj = 0;
uint8_t       IndexSaveMsgObj = 0;


/* Счётчик отброшенных сообщений при переполнении очереди (для диагностики) */
static uint32_t SendOverflowCount = 0;

uint8_t USBSndBuf[CDCPKTLEN] = {CDCPRE1, CDCPRE2};

uint8_t CanStopSend = 0;
uint8_t CanStopRetranslate = 0;

uint8_t GetRetranslate() {
	return CanStopRetranslate;
}

/* Вызывается при переполнении очереди отправки; в приложении можно переопределить */
__attribute__((weak)) void CanSendOverError(void) { (void)0; }

uint32_t BackendGetSendOverflowCount(void) {
	return SendOverflowCount;
}

uint8_t BackendGetDeviceCount(void) {
	return nDevs;
}

uint8_t *SavedCfgptr; // указатель на сохранённый массив конфигурации
uint8_t *LocalCfgptr; // указатель на локальный (временный) массив конфигурации



void SendMessageFull(can_ext_id_t can_id, uint8_t *Data, uint8_t Now, uint8_t bus) {
    if (Now) {
		/* Отправка мимо очереди: пишем в текущий слот и сразу шлём */
		BufSendMsgObj[IndexSaveMsgObj].ui32MsgID = can_id.ID;
		memcpy(&BufSendMsgObj[IndexSaveMsgObj].pui8MsgData, Data, 8);
		BufSendMsgObj[IndexSaveMsgObj].bus = bus;
		CANSendData((uint8_t *)&BufSendMsgObj[IndexSaveMsgObj]);
		return;
	}

	/* При переполнении сдвигаем голову очереди — затираем старые пакеты новыми */
	uint8_t next_save = IndexSaveMsgObj + 1;
	if (next_save >= NumSendMsgObj)
		next_save = 0;
	if (next_save == IndexSendMsgObj) {
		SendOverflowCount++;
		CanSendOverError();
		IndexSendMsgObj++;
		if (IndexSendMsgObj >= NumSendMsgObj)
			IndexSendMsgObj = 0;
	}

	BufSendMsgObj[IndexSaveMsgObj].ui32MsgID = can_id.ID;
	memcpy(&BufSendMsgObj[IndexSaveMsgObj].pui8MsgData, Data, 8);
	BufSendMsgObj[IndexSaveMsgObj].bus = bus;
	IndexSaveMsgObj = next_save;
}

void BackendProcess() {


	/* отправка сообщений в кан из циклического буфера, 1 сообщение каждые SEND_DELAY_MS*/
	if(CanStopSend == 1)
		return;
	else
		send_delay++;

    if(send_delay > SEND_DELAY_MS) {
    	send_delay = 0;

		if (IndexSaveMsgObj != IndexSendMsgObj) {
			CANSendData((uint8_t *)&BufSendMsgObj[IndexSendMsgObj]);

			IndexSendMsgObj++;
			if (IndexSendMsgObj >= NumSendMsgObj)
				IndexSendMsgObj = 0;
		}
    }
}

void ProtocolParse(uint32_t MsgID, uint8_t *MsgData, uint8_t bus) {
	uint8_t Buf[8] = {0, 0, 0, 0, 0, 0, 0, 0};
	uint8_t i;
	uint8_t isBroadcast = 0;
	memcpy(Buf, MsgData, sizeof(Buf));

	uint8_t dir = MsgID>>28;


    if(dir == 1) { // посылки от устройств - пересылаем в USB и работа Listener
		if(isListener) {
			ListenerCommandCB(MsgID, Buf);
		}
    }

	/* Разбор ID по текущему протоколу */
	can_ext_id_t id;
	id.ID = MsgID;
	uint8_t Command = Buf[0];

	/* Broadcast-сообщение: адрес (zone, h_adr, l_adr) = 0, тип устройства задан */
	if (/*(id.field.zone == 0) &&*/ (id.field.h_adr == 0) && (id.field.l_adr == 0))
		isBroadcast = 1;
	else
		isBroadcast = 0;

	for(i = 0; i < nDevs; i++)	{
		/* Сообщение предназначено устройству, если совпадает тип,
		 * а также зона и адрес, либо это broadcast по типу. */
		uint8_t type_match = (id.field.d_type == (BoardDevicesList[i].d_type & 0x7F));
		uint8_t addr_match = /*(id.field.zone == (BoardDevicesList[i].zone & 0x7F)) &&*/ /* пока без валидации зоны, адреса достаточно */
		                     (id.field.h_adr == BoardDevicesList[i].h_adr) &&
		                     ((id.field.l_adr & 0x3F) == (BoardDevicesList[i].l_adr & 0x3F));

		if ((type_match && addr_match) || (/*type_match &&*/ isBroadcast)) {


    		if(Command >= 128) {
    			uint8_t *pData = &Buf[1];
    			ServiceCommandParse(0, Command, pData, bus, dir);
    			return;
    		} else {
    			if(dir == 1) return;
                uint8_t *pData = &Buf[1];
                CommandCB(i, Command, pData);
                if(isBroadcast) // если broadcast, цикл пройдёт по другим устройствам (массовая остановка и т.п.)
                	break;
                else
                	return; // иначе завершаем проход по списку

    		}
	    }
	}
}

void SendAllMessage(uint8_t Cmd, uint8_t *Data, uint8_t Now, uint8_t bus) {
	can_ext_id_t can_id;
	uint8_t data[8] = {0, 0, 0, 0, 0, 0, 0, 0};

	data[0] = Cmd;
	memcpy(&data[1], Data, 7);
	can_id.ID = 0;
	can_id.field.dir = 1;
	can_id.field.d_type = 0;
	can_id.field.h_adr = 0;
	can_id.field.l_adr = 0;
	can_id.field.zone = 0;

    SendMessageFull(can_id, data, Now, bus);
}

void SendMessage(uint8_t Dev, uint8_t Cmd, uint8_t *Data, uint8_t Now, uint8_t bus) {
	can_ext_id_t can_id;
	uint8_t data[8] = {0, 0, 0, 0, 0, 0, 0, 0};

	data[0] = Cmd;
	memcpy(&data[1], Data, 7);
	can_id.ID = 0;
	can_id.field.dir = 1;
	can_id.field.d_type = BoardDevicesList[Dev].d_type & 0x7F;
	can_id.field.h_adr = BoardDevicesList[Dev].h_adr;
	can_id.field.l_adr = BoardDevicesList[Dev].l_adr & 0x3F;
	can_id.field.zone = BoardDevicesList[Dev].zone & 0x7F;

    SendMessageFull(can_id, data, Now, bus);
}

void FireServiceCmd(uint8_t Dev, uint8_t Command, uint8_t *MsgData) {
	uint8_t Data[7] = {0, 0, 0, 0, 0, 0, 0};
	switch(Command) {
		case ServiceCmd_SetStatusFire: {
			RcvStatusFire();
		}break;
		case ServiceCmd_ReplyStatusFire: {
			RcvReplyStatusFire();
		}break;
		case ServiceCmd_StartExtinguishment: {
			RcvStartExtinguishment();
		}break;
		case ServiceCmd_StopExtinguishment: {
			RcvStopExtinguishment();
		}break;
	}
}

void ConfigServiceCmd(uint8_t Dev, uint8_t Command, uint8_t *MsgData) {
	uint8_t Data[7] = {0, 0, 0, 0, 0, 0, 0};

	switch(Command) {
		case ServiceCmd_GetConfigSize: { // Get config size ( bytes)
			uint32_t sz = GetConfigSize();
			Data[0] = (sz >> 24 ) & 0xFF;
			Data[1] = (sz >> 16 ) & 0xFF;
			Data[2] = (sz >> 8 ) & 0xFF;
			Data[3] = (sz >> 0 ) & 0xFF;
			SendMessage(Dev, Command, Data, SEND_NOW, BUS_CAN12);
		}break;
		case ServiceCmd_GetConfigCRC: { // вернуть контрольную сумму массива конфигурации
			uint32_t crc = 0;
			if(MsgData[0] == 0)
				crc = crc32(POLYNOM, SavedCfgptr, GetConfigSize());
			else
				crc = crc32(POLYNOM, LocalCfgptr, GetConfigSize());

			for(uint8_t i = 0; i < 4; i++) {
				Data[i] = (crc >> (24 - 8 * i)) & 0xFF;
			}
			SendMessage(Dev, Command, Data, SEND_NOW, BUS_CAN12);
		} break;
		case ServiceCmd_GetConfigWord: {
			uint16_t num_word = 0;

			num_word = MsgData[0];
			num_word <<= 8;
			num_word |= MsgData[1];

			uint32_t word = GetConfigWord(num_word);
			Data[0] = MsgData[0];
			Data[1] = MsgData[1];
			for(uint8_t i = 0; i < 4; i++) {
				Data[i + 2] = (word >> (24 - 8 * i)) & 0xFF;
			}
			SendMessage(Dev, Command, Data, SEND_NOW, BUS_CAN12);
		}break;
		case ServiceCmd_SetConfigWord: {
			uint16_t num_word = 0;

			num_word = MsgData[0];
			num_word <<= 8;
			num_word |= MsgData[1];

			uint32_t word = 0;

			for(uint8_t i = 0; i < 4; i++) {
				word <<= 8;
				word |= MsgData[2 + i];
			}
			SetConfigWord(num_word, word);

			Data[0] = MsgData[0];
			Data[1] = MsgData[1];
			for(uint8_t i = 0; i < 4; i++) {
				Data[i + 2] = (word >> (24 - 8 * i)) & 0xFF;
			}
			SendMessage(Dev, Command, Data, SEND_NOW, BUS_CAN12);
		}break;
		case ServiceCmd_SaveConfig: { // save config from local
			SaveConfig();
		}break;
		case ServiceCmd_DefaultConfig: { // restore defaults into local config
			DefaultConfig();
		}break;
		case ServiceCmd_SetSystemTime: {
			// Установка системного времени реализуется на стороне ППКУ (STM32 RTC),
			// на МКУ дополнительных действий не требуется.
		}break;
	}
}

void ServiceCommandParse(uint8_t Dev, uint8_t Command, uint8_t *MsgData, uint8_t bus, uint8_t dir) {

	switch(Command) {
		case ServiceCmd_ResetMCU: { // Restart MCU
			ResetMCU();
		}break;
		case ServiceCmd_StopStartSend: { // Stop/Start останавливает очередь на отправку в кан, остаются только принудительные (приоритетные отправки)
			CanStopSend = MsgData[0];
		}break;
		case ServiceCmd_StopStartReTranslate: { // Stop/Start останавливает автоматическую ретрансляцию из одного CAN в другой
			CanStopRetranslate = MsgData[0];

		}break;
		case ServiceCmd_CircSetAdr: { // установка адреса по кольцу
			if(isMaster == 1)
				return;

			uint8_t new_adr = MsgData[0];

			SetHAdr(new_adr);

			MsgData[0]++;
			uint8_t reply_bus;
			if(bus == BUS_CAN0)
				reply_bus = BUS_CAN1;
			else
				reply_bus = BUS_CAN0;
			SendAllMessage(Command, MsgData, SEND_NOW, reply_bus);
		}break;
		case ServiceCmd_SetSystemTime: {
			RcvSetSystemTime(MsgData);
		}break;

		case ServiceCmd_SetStatusFire:
		case ServiceCmd_ReplyStatusFire:
		case ServiceCmd_StartExtinguishment:
		case ServiceCmd_StopExtinguishment: {
			FireServiceCmd(Dev, Command, MsgData);
		}break;

		// Work with config data
		case ServiceCmd_GetConfigSize:
		case ServiceCmd_GetConfigCRC:
		case ServiceCmd_GetConfigWord:
		case ServiceCmd_SetConfigWord:
		case ServiceCmd_SaveConfig:
		case ServiceCmd_DefaultConfig: {
			if(dir & (Dev == 0)) // если от нас и нам, то исключаем (кольцо)
				return;
			else
				ConfigServiceCmd(Dev, Command, MsgData);
		}break;

	}
}

void SetConfigPtr(uint8_t *SConfigPtr, uint8_t *LConfigPtr) {
	SavedCfgptr = SConfigPtr; LocalCfgptr = LConfigPtr;
}

void SetStatusFire(uint8_t *Data) {
	SendMessage(0, ServiceCmd_SetStatusFire, Data, 1, BUS_CAN12);
}
void SetReplyStatusFire() {
	uint8_t Data[7] = {0, 0, 0, 0, 0, 0, 0};
	SendMessage(0, ServiceCmd_ReplyStatusFire, Data, 1, BUS_CAN12);
}
void SetStartExtinguishment(uint8_t zone) {
	uint8_t Data[7] = {zone, 0, 0, 0, 0, 0, 0};
	SendMessage(0, ServiceCmd_StartExtinguishment, Data, 1, BUS_CAN12);
}
void SetStopExtinguishment() {
	uint8_t Data[7] = {0, 0, 0, 0, 0, 0, 0};
	SendMessage(0, ServiceCmd_StopExtinguishment, Data, 1, BUS_CAN12);
}


/***********************************************************************************************************
 * Оболочка пакета для посылок по другим интерфейсам
 ***********************************************************************************************************/

uint16_t BSU_Checksum(const uint8_t *data, uint32_t len)
{
	uint32_t sum = 0;
	for (uint32_t i = 0; i < len; i++) {
		sum += data[i];
	}
	return (uint16_t)(sum & 0xFFFFu);
}

uint16_t BSU_PacketBuildCan(uint8_t *out_buf, uint32_t buf_size, uint32_t can_id, const uint8_t *data)
{
	if (out_buf == NULL || data == NULL || buf_size < BSU_PKT_CAN_SIZE) {
		return 0;
	}

	uint16_t pos = 0;

	out_buf[pos++] = BSU_PKT_PREAMBLE_LO;
	out_buf[pos++] = BSU_PKT_PREAMBLE_HI;

	uint16_t pkt_size = BSU_PKT_CAN_SIZE;
	out_buf[pos++] = (uint8_t)(pkt_size & 0xFFu);
	out_buf[pos++] = (uint8_t)(pkt_size >> 8);

	out_buf[pos++] = (uint8_t)(BSU_PKT_TYPE_CAN & 0xFFu);
	out_buf[pos++] = (uint8_t)(BSU_PKT_TYPE_CAN >> 8);

	out_buf[pos++] = 0;  /* seq lo - для CAN всегда 0 */
	out_buf[pos++] = 0;  /* seq hi */

	out_buf[pos++] = (uint8_t)(can_id & 0xFFu);
	out_buf[pos++] = (uint8_t)((can_id >> 8) & 0xFFu);
	out_buf[pos++] = (uint8_t)((can_id >> 16) & 0xFFu);
	out_buf[pos++] = (uint8_t)((can_id >> 24) & 0xFFu);

	memcpy(&out_buf[pos], data, 8);
	pos += 8;

	uint16_t crc = BSU_Checksum(out_buf, pos);
	out_buf[pos++] = (uint8_t)(crc & 0xFFu);
	out_buf[pos++] = (uint8_t)(crc >> 8);

	return (uint16_t)pos;
}

uint8_t BSU_PacketParse(const uint8_t *buf, uint32_t len, uint32_t *out_can_id, uint8_t *out_data)
{
	if (buf == NULL || out_can_id == NULL || out_data == NULL || len < BSU_PKT_CAN_SIZE) {
		return 0;
	}

	if (buf[0] != BSU_PKT_PREAMBLE_LO || buf[1] != BSU_PKT_PREAMBLE_HI) {
		return 0;
	}

	uint16_t pkt_size = (uint16_t)buf[2] | ((uint16_t)buf[3] << 8);
	if (pkt_size != BSU_PKT_CAN_SIZE || len < pkt_size) {
		return 0;
	}

	uint16_t pkt_type = (uint16_t)buf[4] | ((uint16_t)buf[5] << 8);
	if (pkt_type != BSU_PKT_TYPE_CAN) {
		return 0;
	}

	uint16_t calc_crc = BSU_Checksum(buf, pkt_size - BSU_PKT_CHECKSUM_SIZE);
	uint16_t recv_crc = (uint16_t)buf[pkt_size - 2] | ((uint16_t)buf[pkt_size - 1] << 8);
	if (calc_crc != recv_crc) {
		return 0;
	}

	/* payload: 4 байта ID + 8 байт data, начинается с offset 8 */
	*out_can_id = (uint32_t)buf[8] | ((uint32_t)buf[9] << 8) |
	              ((uint32_t)buf[10] << 16) | ((uint32_t)buf[11] << 24);
	memcpy(out_data, &buf[12], 8);

	return 1;
}






