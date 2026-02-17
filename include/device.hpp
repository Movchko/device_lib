
#ifndef PORT_H_
#define PORT_H_

#include <stdint.h>
#include <string.h>
#include <math.h>

#define VDEVICE_CFG_SIZE 64 // байт

#define DEVICE_PPKY_TYPE 10
#define DEVICE_IGNITER_TYPE 11
#define DEVICE_DPT_TYPE 12


enum DeviceState {
	DeviceState_Idle,
	DeviceState_Run,
	DeviceState_Error
};



enum DType {
    DT_None,
    DT_DPT,  /* Датчик превышения температуры */
};


struct VDeviceCfg {

    /* . резерв нужен чтобы бесшовно обновлять устройство с имзенением структуры,
     * при этом резерв уменьшать на кол-во давленных новых данных
     */
    uint8_t reserv[VDEVICE_CFG_SIZE]; //
};


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
