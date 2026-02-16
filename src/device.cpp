#include <device.hpp>


VDevice::VDevice(uint8_t ch) {
	Num = ch;
	isDeviceInit = 0;
	DeviceType = DT_None;
}

void VDevice::SetStatus() {
	/* базовая реализация ничего не делает */
}

void VDevice::SetDefaultCfg() {

}

void VDevice::Process() {

}

void VDevice::CommandCB(uint8_t Command, uint8_t *Parameters) {
	/* базовая реализация команды игнорирует */
	(void)Command;
	(void)Parameters;
}

void VDevice::Init() {
	/* базовая инициализация ничего не делает */
}

void VDevice::DeviceInit(VDeviceCfg *ConfigPtr) {
	CfgPtr = ConfigPtr;
}

void VDevice::Timer1ms() {

}

void VDevice::Timer1us() {

}

