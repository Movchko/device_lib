# Протокол RS485: ППКУ 2 ↔ Панель (RSP — RS Panel)

**Версия спецификации:** 0.3  
**Статус:** проект, к реализации не приступали  
**Дата:** 2026-07-13

---

## 1. Назначение

Протокол описывает обмен по шине RS485 между **ППКУ 2** (master) и одной или несколькими **Панелями** (slave). Панель — устройство отображения и ввода: дисплей (TouchGFX), кнопки, светодиоды, звук. Логика пожаротушения, сценариев, неисправностей, конфигурации и логирования остаётся в ППКУ 2.

### 1.1. Разделение ответственности

| Компонент | ППКУ 2 | Панель |
|-----------|--------|--------|
| CAN-шина, МКУ | ✓ | — |
| FSM пожара / тушения | ✓ | — |
| Очередь неисправностей (логика) | ✓ | — |
| Winbond Flash (конфиг, лог) | ✓ | — |
| ESP32 / WiFi | ✓ | — |
| Дисплей, TouchGFX | команды | ✓ (вёрстка, отрисовка) |
| Кнопки (чтение, дебаунс) | — | ✓ |
| LED, звук (исполнение) | команды | ✓ |
| Содержимое меню, значения конфига | ✓ | — |
| Внешний вид экранов | — | ✓ |

### 1.2. Принципы протокола

1. **ППКУ 2 — единственный master**, инициирует все транзакции.
2. **Семантические UI-команды** — ППКУ говорит «открой меню Настройки», панель знает, как он выглядит.
3. **Типизированные кнопки и LED** — не фиксированные битовые маски; состав зависит от варианта панели.
4. **Несколько панелей** на одной шине (до 8 одновременно).
5. **Текст для экрана готовит ППКУ 2** — панель не знает типов МКУ, зон CAN и т.п.
6. Другие типы RS-устройств в этой версии **не закладываются**.

---

## 2. Физический уровень

| Параметр | Значение |
|----------|----------|
| Интерфейс | RS485 half-duplex |
| Скорость по умолчанию | **460800** бод (минимум в 2 раза выше прежнего 115200) |
| Допустимые скорости | 460800, 921600 (из конфига `PPKYCfg.ex_rs485_baudrate`) |
| Чётность | 8N1 |
| Преамбула кадра | `0xA5 0x5A` (отличается от BSU `0x55 0xAA` и CAN) |
| Контрольная сумма | CRC16 — тот же алгоритм, что `BSU_Checksum()` в `device_lib` |
| Таймаут ответа панели | 5 мс |
| Watchdog панели | нет ответа 500 мс → неисправность, панель блокируется для команд |

---

## 3. Формат кадра

```
┌──────┬─────┬──────┬─────┬───────┬─────┬─────────┬───────┐
│ PRE  │ LEN │ ADDR │ SEQ │ FLAGS │ CMD │ PAYLOAD │ CRC16 │
│ 2 B  │ 1 B │ 1 B  │ 1 B │  1 B  │ 1 B │ 0..N B  │  2 B  │
└──────┴─────┴──────┴─────┴───────┴─────┴─────────┴───────┘
```

| Поле | Описание |
|------|----------|
| `PRE` | `0xA5`, `0x5A` |
| `LEN` | Количество байт от `ADDR` до конца `PAYLOAD` (без PRE, LEN, CRC) |
| `ADDR` | Адрес назначения (см. §4) |
| `SEQ` | Счётчик кадров master; панель эхоирует в ответе |
| `FLAGS` | Битовые флаги (см. §3.1) |
| `CMD` | Код команды или ответа |
| `PAYLOAD` | Данные команды |
| `CRC16` | Little-endian, по всем байтам от `ADDR` до конца `PAYLOAD` |

Максимальный размер `PAYLOAD`: **512 байт** (с запасом под фрагментацию).

### 3.1. FLAGS

| Бит | Имя | Описание |
|-----|-----|----------|
| 0 | `DIR` | `0` = запрос master→slave, `1` = ответ slave→master |
| 1 | `MORE` | `1` = есть продолжение (фрагмент потока данных) |
| 2 | `FRAG` | `1` = этот кадр — фрагмент; в начале payload: `frag_id u8`, `frag_idx u8`, `frag_total u8` |
| 7 | `ACK_REQ` | `1` = требуется подтверждение `RSP_ACK` в следующем ответе панели |

### 3.2. Фрагментация

Используется для длинных `UI_DATA` (журнал, списки устройств, длинные тексты).

```
При FLAGS.FRAG=1, начало PAYLOAD:
  frag_id    u8   идентификатор потока (увеличивается master)
  frag_idx   u8   номер фрагмента (0..frag_total-1)
  frag_total u8   всего фрагментов
  data[]     ...  полезная нагрузка фрагмента
```

Панель собирает все фрагменты с одинаковым `frag_id` и применяет данные после получения последнего (`frag_idx == frag_total - 1`).

При `FLAGS.MORE=1` без `FRAG` — логическое продолжение команды (например, следующий блок `WARN_ITEM`).

---

## 4. Адресация

| ADDR | Назначение |
|------|------------|
| `0x00` | **Broadcast** — UI, LED, SOUND, TIME, сброс (всем панелям) |
| `0x01..0xFE` | **Unicast** — конкретная панель (POLL, CAPS, PROFILE_SET) |
| `0xFF` | Зарезервирован |

### 4.1. Лимит панелей

На шине одновременно — **не более 8 панелей**. Каждая имеет уникальный `addr` в конфиге ППКУ 2.

### 4.2. Конфиг панелей в ППКУ 2

```c
#define MAX_RS_PANELS  8

typedef struct {
    uint8_t  addr;       // 0x01..0xFE, уникальный
    uint8_t  role;       // 0=обычная, 1=primary (опционально, для ускоренного POLL)
    uint8_t  poll_ms;   // период опроса, по умолчанию 10
    uint8_t  flags;     // bit0=enabled
    uint16_t hw_id;     // ожидаемый идентификатор платы (см. §5.3)
    UniqId   uid;       // сверка при HELLO
    uint8_t  profile_override[16]; // сохранённый оверрайд ориентации/масок
} RsPanelCfg;
```

### 4.3. Планировщик master

```
Каждые poll_ms (по умолчанию 10 мс) — round-robin POLL по всем enabled панелям.
  При 3 панелях: цикл опроса каждой ≈ 30 мс.

Между POLL / в том же 10 мс тике:
  - broadcast: UI_NAV, UI_DATA, LED, SOUND, TIME
  - unicast: если у панели отличается CAPS (другой набор кнопок/LED)
```

Панель с `role=primary` может опрашиваться каждые 10 мс независимо; остальные — реже (20–30 мс).

---

## 5. Handshake и возможности панели (CAPS)

### 5.1. Процедура подключения

```
1. ППКУ 2 → панель: CAPS_REQ (unicast)
2. Панель → ППКУ 2: RSP_CAPS
3. ППКУ 2 сверяет hw_id, uid, состав кнопок/LED с RsPanelCfg
4. При расхождении hw_id → панель БЛОКИРУЕТСЯ (команды не принимаются, неисправность в warning)
5. При успехе → панель в рабочем состоянии
```

Повтор `CAPS_REQ` — при сбое, по таймеру, после `PROFILE_SET`.

### 5.2. RSP_CAPS (ответ панели, CMD=0xF0, DIR=1)

```
fw_ver          u16     версия прошивки панели
hw_id           u16     идентификатор платы (зашит в прошивку)
ui_profile      u8      базовый UI-профиль (0=гориз., 1=верт., 2=другой дисплей)
orientation     u8      текущая ориентация (0=гориз., 1=верт.)
disp_w          u16     ширина дисплея, px
disp_h          u16     высота дисплея, px
journal_lines   u8      сколько строк журнала помещается на экране (1..N)
btn_count       u8
btn_list[]      btn_count × u8   // RsBtnType
led_count       u8
led_list[]      led_count × u8   // RsLedType
flags           u8      bit0=has_sound, bit1=has_beeper
status          u8      bit0=btn_reader_ok, bit1=display_ok
```

### 5.3. Блокировка при расхождении hw_id

Если `RSP_CAPS.hw_id != RsPanelCfg.hw_id` для данного `addr`:

- ППКУ 2 переводит панель в состояние **BLOCKED**.
- Команды UI/LED/SOUND на эту панель **не отправляются**.
- В очередь неисправностей ППКУ добавляется предупреждение «Несоответствие панели».
- Разблокировка — только после совпадения hw_id (замена панели / исправление конфига) и успешного CAPS_REQ.

### 5.4. Изменение профиля в runtime — PROFILE_SET (0xF1)

Unicast. Позволяет изменить ориентацию и состав кнопок/LED без перепрошивки.

```
sub     u8
payload зависит от sub:
```

| sub | Payload | Действие |
|-----|---------|----------|
| `0x01` | `orientation u8` | Поворот UI (TouchGFX orientation) |
| `0x02` | `btn_enable u8` | Битовая маска по RsBtnType (бит N = тип N+1) |
| `0x03` | `led_enable u16` | Битовая маска по RsLedType |
| `0x04` | `journal_lines u8` | Пересчёт вместимости журнала |
| `0x0F` | — | Сброс к заводскому CAPS |

Панель отвечает `RSP_CAPS` (обновлённый) или `RSP_ACK`. ППКУ 2 сохраняет оверрайд в `RsPanelCfg.profile_override`.

Отключённая кнопка не генерирует событий. Отключённый LED игнорирует `CMD_LED` для своего типа.

---

## 6. Типы кнопок и LED

Элементы задаются **семантическим типом**, а не фиксированным индексом. Отсутствующий тип не передаётся в `btn_list` / `led_list`.

### 6.1. RsBtnType

| Код | Имя | Описание |
|-----|-----|----------|
| `0x01` | `RS_BTN_ESC` | ESC / назад |
| `0x02` | `RS_BTN_UP` | Вверх |
| `0x03` | `RS_BTN_DOWN` | Вниз |
| `0x04` | `RS_BTN_ENTER` | Enter / подтверждение |
| `0x05` | `RS_BTN_STOP` | ОСТАНОВ ПУСКА |
| `0x06` | `RS_BTN_START_SP` | ПУСК СП |
| `0x07` | `RS_BTN_START_ALL` | ПУСК ОБЩИЙ |

### 6.2. Состояние кнопки (ButtonState)

Совпадает с текущим `button.h` в ППКУ:

| Значение | Имя | Описание |
|----------|-----|----------|
| `0` | `Reset` | Не нажата |
| `1` | `Press` | Фронт нажатия (короткое) |
| `2` | `LongPress` | Длинное нажатие |
| `3` | `Error` | Ошибка чтения |

### 6.3. RsLedType

| Код | Имя | Описание |
|-----|-----|----------|
| `0x10` | `RS_LED_POWER` | Питание |
| `0x11` | `RS_LED_NORM` | Норма |
| `0x12` | `RS_LED_START` | Пуск |
| `0x13` | `RS_LED_STOP` | Стоп |
| `0x14` | `RS_LED_ERR` | Неисправность |
| `0x15` | `RS_LED_FIRE` | Пожар |
| `0x16` | `RS_LED_AUTO_OFF` | Автоматика отключена |
| `0x20` | `RS_LED_BUT_START_ALL` | Подсветка кнопки ПУСК ОБЩИЙ |
| `0x21` | `RS_LED_BUT_STOP` | Подсветка кнопки СТОП |
| `0x22` | `RS_LED_BUT_START_SP` | Подсветка кнопки ПУСК СП |
| `0x23` | `RS_LED_BUT_ENTER` | Подсветка кнопки ENTER |
| `0x24` | `RS_LED_BUT_ESC` | Подсветка кнопки ESC |
| `0x30` | `RS_LED_LBL_START_ALL` | Подсветка подписи ПУСК ОБЩИЙ |
| `0x31` | `RS_LED_LBL_STOP` | Подсветка подписи СТОП |
| `0x32` | `RS_LED_LBL_START_SP` | Подсветка подписи ПУСК СП |

### 6.4. Режим LED (в CMD_LED)

| mode | Описание |
|------|----------|
| `0` | OFF |
| `1` | ON |
| `2` | BLINK |
| `3` | BRIGHT (value = яркость 0..255) |

---

## 7. Коды команд

### 7.1. Master → Panel (DIR=0)

| CMD | Код | ADDR | Описание |
|-----|-----|------|----------|
| `POLL` | `0x01` | unicast | Периодический опрос |
| `LED` | `0x20` | bcast/ucast | Управление LED |
| `SOUND` | `0x21` | bcast/ucast | Звуковой профиль |
| `UI_NAV` | `0x30` | bcast/ucast | Навигация по экранам |
| `UI_DATA` | `0x31` | bcast/ucast | Данные для текущего экрана |
| `TIME` | `0x32` | broadcast | Синхронизация RTC |
| `CAPS_REQ` | `0xF0` | unicast | Запрос возможностей |
| `PROFILE_SET` | `0xF1` | unicast | Изменение профиля |
| `PANEL_RESET` | `0xF2` | bcast/ucast | Мягкий сброс UI панели |

### 7.2. Panel → Master (DIR=1)

| CMD | Код | Описание |
|-----|-----|----------|
| `RSP_POLL` | `0x81` | Ответ на POLL |
| `RSP_CAPS` | `0xF0` | Возможности панели |
| `RSP_ACK` | `0xFE` | Подтверждение (seq в payload) |

---

## 8. POLL — периодический обмен

### 8.1. POLL (master → panel)

```
flags       u8
  bit0: sound_mute     глобальное отключение звука (из PPKYConfig.beep)
  bit1: config_session блокировка пожарного UI (режим конфигурирования)
  bit2: panel_blocked  панель заблокирована (при hw_id mismatch)
ack_seq     u8         seq последнего принятого RSP_POLL
```

### 8.2. RSP_POLL (panel → master)

```
status          u8
  bit0: btn_reader_ok
  bit1: display_ok
evt_count       u8
btn_events[]    evt_count × (type u8, state u8, level u8)
  type   — RsBtnType
  state  — ButtonState
  level  — физически нажата (0/1); для START_ALL — удержание на ППКУ 2
ui_evt_count    u8
ui_events[]     ui_evt_count × (evt_type u8, p1 u16, p2 u16)
```

Источник события определяется полем `ADDR` в заголовке кадра.

### 8.3. Дедупликация нажатий

Для пожарных кнопок (`RS_BTN_START_SP`, `RS_BTN_START_ALL`, `RS_BTN_STOP`):

- **Действие** (запуск FSM): дедупликация **500 мс** по типу кнопки глобально (с любой панели). Система отрабатывает **первое** нажатие.
- **Логирование**: каждое нажатие с каждой панели записывается в `EventLog` с указанием `panel_addr`.

Для навигационных кнопок (ESC, UP, DOWN, ENTER) дедупликация **не применяется** — события привязаны к `panel_addr` сессии.

### 8.4. Обработка кнопок на ППКУ 2

| Тип | Логика на ППКУ 2 |
|-----|-------------------|
| `RS_BTN_START_ALL` | По `level`: удержание 3 с → `FIRE_EVENT_BTN_START_ALL` (как `Fire_Timer10ms`) |
| `RS_BTN_START_SP` | По `state == Press` → `FIRE_EVENT_BTN_START_SP` |
| `RS_BTN_STOP` | По `state == Press` → `FIRE_EVENT_BTN_STOP` |
| `RS_BTN_UP/DOWN/ESC/ENTER` | UI-события в контексте текущего экрана (меню, журнал, главный) |

---

## 9. UI — семантическая навигация

Панель содержит TouchGFX-экраны. ППКУ 2 управляет **какой экран открыт** и **какие данные на нём показать**.

### 9.1. UI_NAV (0x30)

```
screen_id   u16
action      u8
param       u16
```

**action:**

| Значение | Имя | Описание |
|----------|-----|----------|
| `0` | `OPEN` | Открыть экран (push) |
| `1` | `CLOSE` | Закрыть текущий |
| `2` | `BACK` | Назад |
| `3` | `REPLACE` | Заменить (принудительно, напр. при пожаре) |

**screen_id:**

| ID | Экран |
|----|-------|
| `0x0000` | LOGO |
| `0x0001` | MAIN (главный: пожар / неисправности) |
| `0x0010` | MENU_ROOT |
| `0x0011` | MENU_SETTINGS |
| `0x0012` | MENU_DEVICES |
| `0x0013` | MENU_DEVICE_DETAIL |
| `0x0014` | MENU_CONFIG |
| `0x0015` | MENU_JOURNAL |
| `0x0016` | MENU_JOURNAL_DETAIL |
| `0x0017` | MENU_CONNECTION |
| `0x0018` | MENU_SOUND |
| `0x00FF` | BLANK |

При активном пожаре ППКУ 2 отправляет `UI_NAV(REPLACE, MAIN)` на **broadcast**.

### 9.2. UI_DATA (0x31)

```
sub_id   u8
payload  ...
```

| sub_id | Имя | Назначение |
|--------|-----|------------|
| `0x01` | `MAIN_FIRE` | Состояние пожарного экрана |
| `0x02` | `MAIN_WARN` | Список неисправностей |
| `0x03` | `DATETIME` | Дата/время на экране |
| `0x10` | `MENU_LIST` | Пункты меню + выделение |
| `0x11` | `MENU_VALUE` | Пара label + value |
| `0x12` | `MENU_TOGGLE` | Переключатель on/off |
| `0x20` | `JOURNAL_LIST` | Страница журнала |
| `0x21` | `JOURNAL_DETAIL` | Детали записи журнала |
| `0x22` | `DEVICE_LIST` | Список устройств |
| `0x23` | `DEVICE_DETAIL` | Детали устройства |
| `0x30` | `CONNECTION_STATUS` | Статус WiFi/ESP32 |

### 9.3. Форматы UI_DATA payload

#### MAIN_FIRE (0x01)

```
active       u8     bit0 = пожар активен
mode         u8     0=idle, 1=ДО ПУСКА, 2=ТУШЕНИЕ, 3=ТУШ.ВЫП., 4=ПОЖАР/ОСТ.,
                    5=ПАУЗА, 6=ПОЖАР1, 7=ТУШ.ОШ.
remaining_s  u8     секунды до автопуска
sel_index    u8     выбранная зона (навигация UP/DOWN)
n_zones      u8
zones[]      n_zones × (len u8 + utf8[len])   len ≤ 32
```

#### MAIN_WARN (0x02)

```
count        u8
ver          u8     версия списка (инкремент при изменении)
crc16        u16    CRC по compact-представлению списка
n_items      u8
items[]      n_items ×:
               flags u8   bit0=ВНИМАНИЕ
               title_len u8 + title[title_len]   max 24
               detail_len u8 + detail[detail_len] max 48
```

При большом списке — фрагментация (`FLAGS.FRAG`). Панель крутит ротацию строк локально (анимация).

#### MENU_LIST (0x10)

```
selected     u16
n_items      u8
items[]      n_items × (len u8 + utf8[len])
```

#### JOURNAL_LIST (0x20)

```
total           u32   всего записей в логе
selected_idx    u32   глобальный индекс выделенной записи
window_first    u32   индекс первой видимой строки
n_items         u8    строк в пакете (1..journal_lines)
items[]         n_items ×:
  rec_idx       u32
  ts            u32   Unix timestamp (compact)
  code          u16   код из event_log_catalog
  text_len      u8
  text[text_len]      краткий текст, max 48
```

Навигация:

| Событие | Действие ППКУ 2 |
|---------|-----------------|
| UP | `selected_idx--`, сдвиг `window_first` при необходимости |
| DOWN | `selected_idx++`, сдвиг окна |
| ENTER | открыть `JOURNAL_DETAIL` для `rec_idx == selected_idx` |
| ESC | `UI_NAV(BACK)` |

После каждого изменения — новый `JOURNAL_LIST`.

#### JOURNAL_DETAIL (0x21)

```
rec_idx     u32
ts          u32
code        u16
text_len    u16
text[]      полный текст
raw_len     u8
raw[]       опционально сырые поля записи (до 32 байт)
```

#### CONNECTION_STATUS (0x30)

```
wifi_enabled  u8
esp_online    u8
wifi_block    u8   из PPKYConfig.wifi_block
```

### 9.4. UI-события (panel → master, в RSP_POLL.ui_events)

| evt_type | Имя | p1 | p2 |
|----------|-----|----|----|
| `0x01` | `UI_EVT_NAV` | направление: 0=UP, 1=DOWN | текущий selected |
| `0x02` | `UI_EVT_CONFIRM` | — | — |
| `0x03` | `UI_EVT_BACK` | — | — |
| `0x04` | `UI_EVT_MENU_SELECT` | index пункта | — |
| `0x05` | `UI_EVT_JOURNAL_OPEN` | rec_idx | — |

---

## 10. Меню и конфигурация — state machine на ППКУ 2

Панель **не хранит конфиг**. ППКУ 2 ведёт сессию:

```c
struct UiSession {
    uint16_t screen_id;
    uint16_t selected;
    uint8_t  panel_addr;    // источник последнего ввода
    uint8_t  mcu_slot;
    uint32_t journal_sel;
    uint32_t journal_window;
};
```

### 10.1. Пример: смена режима тушения

```
1. ENTER на главном → ППКУ: UI_NAV(OPEN, MENU_ROOT)
                     → UI_DATA(MENU_LIST, ["Настройки","Устройства","Журнал",...])
2. Пользователь выбирает "Настройки" → UI_EVT_MENU_SELECT
   ППКУ: UI_NAV(OPEN, MENU_SETTINGS)
       → UI_DATA(MENU_LIST, [...])
3. Выбор "Режим тушения" → UI_NAV + UI_DATA(MENU_LIST, ["Авто","Автономный","Ручной"])
4. UP/DOWN → UI_EVT_NAV → ППКУ меняет selected
5. ENTER → UI_EVT_CONFIRM
   ППКУ: PPKYConfig.fire_mode = selected; SaveConfig(); EventLog_Post(...)
```

Конфигурация МКУ, зон, WiFi — аналогично: данные из Flash/CAN-кэша ППКУ 2, панель только отображает и пересылает события.

---

## 11. LED и звук

### 11.1. CMD_LED (0x20)

```
count   u8
items[] count × (type u8, mode u8, value u8)
```

TLV-список по RsLedType. Broadcast — всем панелям с одинаковым CAPS; unicast — при разном составе LED.

### 11.2. CMD_SOUND (0x21)

```
profile   u8
mute      u8   bit0
```

**profile:**

| Значение | Профиль |
|----------|---------|
| `0` | OFF |
| `1` | FAULT (неисправность) |
| `2` | ATTN (внимание) |
| `3` | FIRE (пожар2) |
| `4` | FIRE1 (пожар1) |
| `5` | START (пуск) |
| `6` | START_ALL_HOLD (удержание ПУСК ОБЩИЙ) |
| `7` | BTN_ACK (квитанция кнопки) |
| `8` | CUSTOM |

При `CUSTOM` дополнительно:

```
on_ms       u16
off_ms      u16
pulses      u8
repeat_ms   u16
```

Тайминги соответствуют `sound_profiles.h` в прошивке ППКУ.

### 11.3. CMD_TIME (0x32)

```
hour    u8
min     u8
sec     u8
day     u8
month   u8
year    u8   (offset от 2000)
```

Broadcast всем панелям.

---

## 12. Несколько панелей — синхронизация

```
                    ┌─────────────┐
                    │   ППКУ 2    │
                    └──────┬──────┘
           ┌───────────────┼───────────────┐
           ▼               ▼               ▼
      [Панель 0x01]  [Панель 0x02]  [Панель 0x03]
```

| Тип данных | Режим | Примечание |
|------------|-------|------------|
| POLL | unicast | Каждая панель опрашивается отдельно |
| UI_NAV, UI_DATA (пожар, аварии) | broadcast | Все панели показывают одно и то же |
| LED, SOUND | broadcast / unicast | unicast при разном CAPS |
| TIME | broadcast | |
| PROFILE_SET | unicast | Только целевая панель |

При пожаре: `UI_NAV(REPLACE, MAIN)` + `UI_DATA(MAIN_FIRE)` на broadcast.

Кнопка «ПУСК СП» с панели 0x02: `RSP_POLL` с `ADDR=0x02` → ППКУ обрабатывает с дедупликацией и логирует `panel_addr=0x02`.

---

## 13. Неисправности панели

| Условие | Реакция ППКУ 2 |
|---------|----------------|
| Нет ответа 500 мс | Неисправность связи с панелью `addr` |
| `hw_id` mismatch | **Блокировка** панели |
| `btn_reader_ok=0` | Неисправность кнопок панели |
| `display_ok=0` | Неисправность дисплея |

Тушение и сценарии **не зависят** от наличия панели.

---

## 14. Примеры обмена

### 14.1. Старт системы

```
ППКУ → 01: CAPS_REQ
01 → ППКУ: RSP_CAPS (hw_id=0x0103, btn: ESC,UP,DOWN,ENTER,STOP,START_SP, journal_lines=4)
ППКУ: сверка OK

ППКУ → 02: CAPS_REQ
02 → ППКУ: RSP_CAPS (hw_id=0x9999, ...)
ППКУ: hw_id mismatch → BLOCK panel 02
```

### 14.2. Дежурный режим (10 мс тик)

```
ППКУ → 01: POLL {mute=0, config_session=0}
01 → ППКУ: RSP_POLL {evt_count=0, ui_evt_count=0}

ППКУ → 02: POLL {panel_blocked=1}
02 → ППКУ: RSP_POLL {status=...}  // команды UI не отправляются
```

### 14.3. Пожар

```
[CAN: пожар в зоне 3]
ППКУ → 00 (broadcast): UI_NAV(REPLACE, MAIN)
ППКУ → 00: UI_DATA(MAIN_FIRE, active=1, mode=1, remaining_s=30, zones=["Зона 3"])
ППКУ → 00: LED [(RS_LED_FIRE,ON), (RS_LED_BUT_START_SP,BRIGHT,255)]
ППКУ → 00: SOUND(FIRE)

01 → ППКУ: RSP_POLL {BTN_START_SP, Press}
ППКУ: fire.c → FIRE_EVENT_BTN_START_SP (первое нажатие)
ППКУ: EventLog(panel=01, BTN_START_SP)

02 → ППКУ: RSP_POLL {BTN_START_SP, Press}  // в течение 500 мс
ППКУ: дедупликация → FSM не дублируется
ППКУ: EventLog(panel=02, BTN_START_SP)     // но лог пишется
```

### 14.4. Журнал

```
ППКУ → 00: UI_NAV(OPEN, MENU_JOURNAL)
ППКУ → 00: UI_DATA(JOURNAL_LIST, total=1200, selected_idx=1199,
                     window_first=1196, n_items=4, items=[...])

Пользователь на панели 01: DOWN
01 → ППКУ: UI_EVT_NAV(DOWN, selected=1199)
ППКУ: selected_idx=1198, пересчёт window
ППКУ → 00: UI_DATA(JOURNAL_LIST, selected_idx=1198, ...)

ENTER:
01 → ППКУ: UI_EVT_JOURNAL_OPEN(rec_idx=1198)
ППКУ → 00: UI_NAV(OPEN, MENU_JOURNAL_DETAIL)
ППКУ → 00: UI_DATA(JOURNAL_DETAIL, rec_idx=1198, text="Пожар зона 3 ...")
```

---

## 15. Связь с текущей прошивкой ППКУ (stm_PPKY)

При разделении на ППКУ 2 + Панель модули распределяются так:

| Модуль stm_PPKY | ППКУ 2 | Панель |
|-----------------|--------|--------|
| `can_bus.c`, `app.cpp` | ✓ | — |
| `fire.c` | ✓ | — |
| `warning.cpp` (логика) | ✓ | — |
| `event_log*` | ✓ | — |
| `config*.cpp` | ✓ | — |
| `menu_ui.c` (сессия) | ✓ | — |
| ESP32 / UART2 | ✓ | — |
| Winbond / `spif.c` | ✓ | — |
| `button.c` | — | ✓ |
| `led.c` | — | ✓ |
| `beeper.c` | — | ✓ |
| `WEO012864A.c` + TouchGFX | — | ✓ |
| `Fire_UiUpdate` / `Warning_UiUpdate` | генерация `UI_DATA` | приём и отображение |

---

## 16. Зарезервировано / вне scope v0.3

- Другие типы RS-устройств (не панели).
- Прокси CAN через RS485.
- Обновление прошивки панели по RS485.
- Шифрование / аутентификация.

---

## 17. История версий

| Версия | Дата | Изменения |
|--------|------|-----------|
| 0.1 | 2026-07-09 | Первичный черновик: POLL, FIRE_UI, WARN, LED, SOUND |
| 0.2 | 2026-07-09 | Типы кнопок/LED, семантический UI, CAPS, несколько панелей |
| 0.3 | 2026-07-13 | Журнал постранично, state machine меню, PROFILE_SET, блокировка hw_id, дедупликация 500 мс, 460800 бод, фрагментация, итоговая спецификация |
