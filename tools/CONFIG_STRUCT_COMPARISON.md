# Сравнение структур конфигурации: bus_monitor vs device_lib

## 1. UniqId (backend.h)

**device_lib:**
```c
typedef struct {
    uint32_t  UId0;   // offset 0,  4 байта
    uint32_t  UId1;   // offset 4,  4 байта
    uint32_t  UId2;   // offset 8,  4 байта
    uint32_t  UId3;   // offset 12, 4 байта
    uint32_t  UId4;   // offset 16, 4 байта
    Device    devId;  // offset 20, 4 байта (zone, l_adr, h_adr, d_type)
    uint8_t   reserv[16];  // UNIQ_ID_SIZE(32) - 16
} UniqId;  // итого 32 байта
```

**bus_monitor:** использует offset 20–23 для devId ✓ **совпадает**

---

## 2. Device (backend.h)

**device_lib:**
```c
typedef struct {
    uint8_t zone;    // offset 0
    uint8_t l_adr;   // offset 1
    uint8_t h_adr;   // offset 2
    uint8_t d_type;  // offset 3
} Device;  // 4 байта
```

**bus_monitor:** zone=+20, l_adr=+21, h_adr=+22, d_type=+23 ✓ **совпадает**

---

## 3. VDeviceCfg (device_config.h)

### C-режим (device_config.h, без device.hpp):
```c
typedef struct {
    uint8_t type;   // offset 0, 1 байт
    uint8_t reserv[63];  // VDEVICE_CFG_SIZE(64) - 1
} VDeviceCfg;  // 64 байта
```

### C++-режим (device.hpp):
```cpp
struct VDeviceCfg {
    DType type;   // offset 0, enum = 4 байта на ARM!
    uint8_t reserv[63];
};  // 67 байт
```

**bus_monitor:** Devices[0].type = offset 32 ✓  
**Важно:** В C++ (stm_PPKY) VDeviceCfg = 67 байт → sizeof(MKUCfg) другой!

---

## 4. MKUCfg (device_config.h)

**device_lib:**
```c
typedef struct MKUCfg {
    UniqId     UId;           // 32 байта, offset 0
    VDeviceCfg Devices[16];   // 16 × 64 = 1024 (C) или 16 × 67 = 1072 (C++)
    uint8_t    reserv[1];     // 1 байт
} MKUCfg;
```

| Режим | sizeof(MKUCfg) | sizeof(PPKYCfg) |
|-------|----------------|-----------------|
| C     | 32+1024+1 = **1057** | 33 + 32×1057 + 6400 = 40257 + padding |
| C++   | 32+1072+1 = **1105** | 33 + 32×1105 + 6400 = 41793 + padding |

**bus_monitor:** MKUCFG_SIZE = (len - 33 - 6400) / 32 — вычисляет из размера ✓  
**BSU_test_board** (C): 40624 байт → MKUCfg ≈ 1068 (с учётом padding компилятора)

---

## 5. PPKYCfg (device_config.h)

**device_lib:**
```c
typedef struct PPKYCfg {
    UniqId  UId;              // offset 0,  32 байта
    uint8_t beep;             // offset 32, 1 байт
    MKUCfg  CfgDevices[32];   // offset 33
    int8_t  zone_name[100][64];  // 6400 байт
    uint8_t reserv[1];
} PPKYCfg;
```

**bus_monitor:**
- beep: cfg[32] ✓
- CfgDevices[i]: off = 33 + i × MKUCFG_SIZE ✓
- zone_name: offset = 33 + 32 × MKUCFG_SIZE ✓

---

## 6. Смещения внутри CfgDevices[i] (MKUCfg)

| Поле | device_lib offset | bus_monitor |
|------|-------------------|-------------|
| UId.devId.zone   | 20 | off+20 ✓ |
| UId.devId.l_adr  | 21 | off+21 ✓ |
| UId.devId.h_adr  | 22 | off+22 ✓ |
| UId.devId.d_type | 23 | off+23 ✓ |
| Devices[0].type  | 32 | off+32 ✓ |
| Devices[0].reserv (DeviceIgniterConfig/DeviceDPTConfig) | 33 | off+33 ✓ |

---

## 7. DeviceIgniterConfig (в VDeviceCfg.reserv)

**device_lib:**
```c
typedef struct DeviceIgniterConfig {
    uint8_t  disable_sc_check;   // offset 0
    uint16_t start_duration_ms;  // offset 1 (little-endian!)
    uint8_t  reserved[61];
} DeviceIgniterConfig;
```

**bus_monitor:** reserv_base = off+33, disable_sc=cfg[reserv_base], start_ms=unpack("<H", cfg[reserv_base+1:reserv_base+3]) ✓

---

## 8. DeviceDPTConfig (в VDeviceCfg.reserv)

**device_lib:**
```c
typedef struct DeviceDPTConfig {
    uint16_t fire_threshold_ohm;    // offset 0
    uint16_t normal_threshold_ohm;  // offset 2
    uint32_t break_threshold_ohm;   // offset 4
    uint16_t resistor_r1_ohm;       // offset 8
    uint16_t resistor_r2_ohm;       // offset 10
    uint16_t supply_voltage_mv;     // offset 12
    uint16_t adc_resolution;        // offset 14
    uint8_t  is_limit_switch;       // offset 16
    uint8_t  reserved[47];
} DeviceDPTConfig;
```

**bus_monitor:** fire_ohm, norm_ohm, break_ohm по offset 0,2,4 ✓ (little-endian)

---

## 9. Константы

| Константа | device_lib | bus_monitor |
|-----------|------------|-------------|
| ZONE_NAME_SIZE | 64 | 64 ✓ |
| ZONE_NUMBER | 100 | 100 ✓ |
| VDEVICE_CFG_SIZE | 64 | — |
| DEVICE_PPKY_TYPE | 10 | 10 ✓ |
| DEVICE_IGNITER_TYPE | 11 | 11 ✓ |
| DEVICE_DPT_TYPE | 12 | 12 ✓ |
| DEVICE_MCU_IGN_TYPE | 13 | 13 ✓ |
| DEVICE_MCU_TC_TYPE | 14 | 14 ✓ |
| DEVICE_BUTTON_TYPE | 15 | 15 ✓ (device.hpp) |
| DEVICE_LSWITCH_TYPE | 16 | 16 ✓ (device.hpp) |

---

## Выводы

1. **Смещения** в bus_monitor совпадают с device_lib для C-режима.
2. **Различие C/C++:** stm_PPKY (C++) может использовать device.hpp, где `VDeviceCfg` = 67 байт (DType 4 байта). Тогда MKUCfg = 1105, а не 1057/1068.
3. **Динамический MKUCFG_SIZE** в bus_monitor корректен — берётся из фактического размера конфига.
4. **Порядок байт:** ARM little-endian — для uint16_t/uint32_t в reserv используется `<H`/`<I` ✓
