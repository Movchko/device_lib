
#ifndef PORT_H_
#define PORT_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <string.h>
#include <math.h>

#define VDEVICE_CFG_SIZE 64 // байт

#define DEVICE_PPKY_TYPE 10

#define DEVICE_IGNITER_TYPE 11
#define DEVICE_DPT_TYPE 12

#define DEVICE_MCU_IGN_TYPE 13
#define DEVICE_MCU_TC_TYPE 14

#define DEVICE_BUTTON_TYPE 15
#define DEVICE_LSWITCH_TYPE 16

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


	/* физические платы */
	DT_PPKY  = DEVICE_PPKY_TYPE, /* БСУ (ППКУ) */
	DT_MCU_IGN = DEVICE_MCU_IGN_TYPE, /* МКУ с 1 пускателем и 1 ДПТ*/
	DT_MCU_TC = DEVICE_MCU_TC_TYPE, /* МКУ с 1 дпт (с MAX) */
};


struct VDeviceCfg {
	DType type;
    /* . резерв нужен чтобы бесшовно обновлять устройство с имзенением структуры,
     * при этом резерв уменьшать на кол-во давленных новых данных
     */
    uint8_t reserv[VDEVICE_CFG_SIZE - 1]; //
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
