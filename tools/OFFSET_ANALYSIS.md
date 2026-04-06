# Анализ смещений для config size = 40360

## Расчёт MKUCFG_SIZE

```
mku_area = 40360 - 33 - 6400 = 33927
MKUCFG_SIZE = 33927 // 32 = 1060
zone_name_offset = 33 + 32*1060 = 33953
```

Проверка: 33953 + 6400 = 40353. Остаток 7 байт (reserv в конце). ✓

## Теоретический sizeof(MKUCfg) из device_config.h

- UniqId: 32 байта
- VDeviceCfg[16]: 16 × 64 = 1024 байта
- reserv[1]: 1 байт
- **Итого: 1057 байт**

Компилятор добавляет padding до выравнивания 4 байта: 1057 → **1060** ✓

## Смещения CfgDevices[i]

| i | off = 33 + i×1060 | devId (off+20..23) | Devices[0].type (off+32) |
|---|-------------------|--------------------|--------------------------|
| 0 | 33 | 53..56 | 65 |
| 1 | 1093 | 1113..1116 | 1125 |
| 2 | 2153 | 2173..2176 | 2185 |

## Возможные источники ошибки

### 1. Несовпадение sizeof(MKUCfg) между проектами
- **BSU_test_board** (C): MKUCfg = 1060 (с padding)
- **stm_PPKY** (C++ с device.hpp): VDeviceCfg может быть 67 байт (DType=4) → MKUCfg = 1105

Если читаете с stm_PPKY, а парсер считает 1060 — смещение накапливается.

### 2. Формула zone_name
Текущая: `zone_name_offset = 33 + 32 * MKUCFG_SIZE`

Если между CfgDevices и zone_name есть дополнительный padding — zone_name сдвинуты, и мы читаем CfgDevices из зоны zone_name.

### 3. Порядок байт в reserv
DeviceIgniterConfig/DeviceDPTConfig используют `struct.unpack("<H", ...)` (little-endian). ARM — little-endian. ✓

### 4. Проверка: запросить слово 14 вручную
Слово 14 = байты 56–59. В CfgDevices[0] это:
- byte 56 = d_type
- byte 57 = первый байт Devices[0].reserv

Ожидаемо: d_type=13 (МКУ_IGN), значит слово должно быть 0x0Dxxxxxx.
