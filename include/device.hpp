
#ifndef PORT_H_
#define PORT_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <string.h>
#include <math.h>
#include "device_cfg_common.h"


enum DeviceState {
	DeviceState_Idle,
	DeviceState_Run,
	DeviceState_Error
};



enum DType {
    DT_None,
	/* виртуальыне устрйоства */
    DT_DPT = DEVICE_DPT_TYPE,  /* Датчик превышения температуры */
	DT_IGN = DEVICE_IGNITER_TYPE,  /* Спичка  */
	DT_BUT = DEVICE_BUTTON_TYPE, /* Кнопка на базе ДПТ */
	DT_LSW = DEVICE_LSWITCH_TYPE, /* концевик на базе дпт */
	DT_REL = DEVICE_RELAY_TYPE, /* реле */


	/* физические платы */
	DT_PPKY  = DEVICE_PPKY_TYPE, /* БСУ (ППКУ) */
	DT_MCU_IGN = DEVICE_MCU_IGN_TYPE, /* МКУ с 1 пускателем и 1 ДПТ*/
	DT_MCU_TC = DEVICE_MCU_TC_TYPE, /* МКУ с 1 дпт (с MAX) */

	DT_MCU_K1 = DEVICE_MCU_K1, /* МКУ с 2 пускателями и 1 ДПТ (с MAX)*/
	DT_MCU_K2 = DEVICE_MCU_K2, /* МКУ  с 3 пускателями */
	DT_MCU_K3 = DEVICE_MCU_K3, /* МКУ  с 2 концевиками и 1 пускателем*/
	DT_MCU_KR = DEVICE_MCU_KR, /* МКУ  с 2 реле*/
};



#ifdef __cplusplus
}
#endif

class VDevice {
	uint8_t GetNum() {return Num;};

	virtual void SetStatus();
public:
	VDevice(uint8_t ch);


	uint8_t 	Num;
	VDeviceCfg 	*CfgPtr;
	uint8_t		isDeviceInit;
	DType		DeviceType;

	void DeviceInit(VDeviceCfg *ConfigPtr);
	virtual void SetDefaultCfg();
	virtual void Timer1ms();
	virtual void Timer1us();
	virtual void CommandCB(uint8_t Command, uint8_t *Parameters);
	virtual void Init();
	virtual void Process();

	void		(*VDeviceSetStatus)(uint8_t DNum, uint8_t Code, const uint8_t *Parameters);
	void		(*VDeviceSaveCfg)(void);


	virtual uint8_t GetDT() {return 1;}
};



#endif
