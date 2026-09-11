#define _CRT_SECURE_NO_WARNINGS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <timeapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ws2_32.lib")

#define APP_TITLE L"BSU CAN Firmware Updater (C)"

#define IDC_COMBO_PORT      101
#define IDC_BTN_REFRESH     102
#define IDC_BTN_CONNECT     103
#define IDC_LIST_DEVICES    104
#define IDC_EDIT_FILE       105
#define IDC_BTN_BROWSE      106
#define IDC_BTN_START       107
#define IDC_BTN_STOP        108
#define IDC_EDIT_LOG        109
#define IDC_PROGRESS        110
#define IDC_BTN_SELECT_TARGET 111
#define IDC_BTN_FORCE_VERSION 112
#define IDC_CHK_VERIFY        113
#define IDC_EDIT_BATCH        114
#define IDC_BTN_COLLECT       115
#define IDC_COMBO_MODE        116
#define IDC_EDIT_WIFI_HOST    117
#define IDC_EDIT_WIFI_PORT    118

#define FW_WORKSPACE_ROOT     L"D:\\work\\stm_workspace\\"
#define FW_COLLECT_DST        L"D:\\work\\stm_workspace\\firmware_MKU"

#define DEFAULT_BATCH_SIZE    8u
#define MIN_BATCH_SIZE        1u
#define MAX_BATCH_SIZE        128u
#define BATCH_SLEEP_MS        1u

#define DEVICE_PPKY_TYPE      10u
#define DEVICE_ESP32_TYPE     11u
#define DEVICE_PANEL_TYPE     30u
#define PPKY_FW_MAX_BYTES     (512u * 1024u)
#define ESP_FW_MAX_BYTES      (960u * 1024u)
#define PANEL_FW_MAX_BYTES    (256u * 1024u)

#define WIFI_DEFAULT_HOST     L"192.168.4.1"
#define WIFI_DEFAULT_PORT     L"23"

#define ACK_WAIT_MS_PPKY_FIRST      35000u
/* Бюджет ожидания пачки: не сумма старых per-word (64×400 мс ≈ 25 с на 1 lost ACK). */
#define ACK_BATCH_WORD_BUDGET_USB   25u
#define ACK_BATCH_WORD_BUDGET_WIFI  60u
#define ACK_BATCH_IDLE_GAP_USB      100u
#define ACK_BATCH_IDLE_GAP_WIFI     350u
/* Когда почти вся пачка пришла — подождать отстающий ACK дольше. */
#define ACK_BATCH_IDLE_GAP_TAIL_USB  300u
#define ACK_BATCH_IDLE_GAP_TAIL_WIFI 1200u
/* Панель: erase/program — короткий idle даёт ложные «Повтор пачки». */
#define ACK_BATCH_IDLE_GAP_PANEL_WIFI      2500u
#define ACK_BATCH_IDLE_GAP_PANEL_TAIL_WIFI 5000u
#define PANEL_WORD_PACE_MS_WIFI     30u
#define PANEL_MAX_BATCH_SIZE        8u
#define PANEL_TRANSMIT_WAIT_MS      20000u
#define ACK_BATCH_MIN_MS            500u
#define ACK_RETRY_GAP_PANEL_MS      150u

#define WM_APP_LOG          (WM_APP + 1)
#define WM_APP_PACKET       (WM_APP + 2)
#define WM_APP_UPD_DONE     (WM_APP + 3)
#define WM_APP_REQ_VERSION  (WM_APP + 4)
#define IDT_VERSION_RETRY   1
#define VERSION_RETRY_MAX   5
#define VERSION_RETRY_MS    2000u

#define BSU_PKT_TYPE_CAN1      0u
#define BSU_PKT_TYPE_ESP_CMD   3u
#define BSU_PKT_TYPE_ESP_CAN   4u
#define BSU_PKT_TYPE_ESP_UART  5u
#define BSU_PREAMBLE0          0x55u
#define BSU_PREAMBLE1          0xAAu
#define BSU_PKT_SIZE_CAN       22u
#define ESP_UART_BODY_MAX      246u
#define BSU_PKT_MAX_SIZE       256u

#define CMD_SET_UPDATE_WORD    156u
#define CMD_UPDATE_TRANSMIT    158u
#define CMD_GET_VERSION        159u
#define CMD_ENTER_BOOTLOADER   0xF3u
#define RS_PANEL_RSP_ACTIVITY  0x82u
#define RS_PANEL_RSP_ACK       0xFEu
#define RS_BUS_PREAMBLE_0      0xA5u
#define RS_BUS_PREAMBLE_1      0x5Au
#define RS_BUS_FLAG_DIR        0x01u
#define RS_BUS_DEV_TYPE_PANEL_APP        0x01u
#define RS_BUS_DEV_TYPE_PANEL_BOOTLOADER 0x02u
#define ENTER_BOOT_WAIT_MS     15000u
#define ACK_RETRY_FIRST_WORD 4
#define ACK_RETRY_NORMAL 8

typedef struct {
    uint8_t d_type, h_adr, l_adr, zone, dir;
} CanIdFields;

typedef struct {
    uint32_t can_id;
    uint8_t data[8];
    uint8_t bus_label;
} PacketInfo;

typedef struct {
    uint8_t d_type, h_adr, l_adr, zone;
    uint32_t last_seen_ms;
    uint8_t version_valid;
    uint32_t version;
    char version_str[80];
    uint8_t version_pkt_next;
    uint8_t rs_dev_type; /* 0x01 app / 0x02 bootloader, только для DEVICE_PANEL_TYPE */
    uint8_t version_req_sent;
    uint8_t version_retries;
    uint32_t version_req_ms;
} DeviceInfo;

typedef struct {
    int active;
    uint32_t batch_start;
    uint32_t batch_len;
    uint32_t expect_idx[MAX_BATCH_SIZE];
    uint32_t expect_word[MAX_BATCH_SIZE];
    uint8_t acked[MAX_BATCH_SIZE];
    uint32_t acked_count;
} AckState;

static HWND g_hwnd = NULL;
static HWND g_hPort = NULL, g_hConnect = NULL, g_hDevices = NULL, g_hFile = NULL;
static HWND g_hStart = NULL, g_hStop = NULL, g_hLog = NULL, g_hProgress = NULL;
static HWND g_hVerify = NULL, g_hBatch = NULL;
static HWND g_hMode = NULL, g_hWifiHost = NULL, g_hWifiPort = NULL;

static HANDLE g_hSerial = INVALID_HANDLE_VALUE;
static SOCKET g_sock = INVALID_SOCKET;
static int g_useTcp = 0;
static HANDLE g_hReaderThread = NULL;
static HANDLE g_hUpdaterThread = NULL;
static volatile LONG g_readerStop = 0;
static volatile LONG g_updateStop = 0;
static volatile LONG g_connected = 0;
static volatile LONG g_updateRunning = 0;

static CRITICAL_SECTION g_ioCs;
static CRITICAL_SECTION g_ackCs;
static CONDITION_VARIABLE g_ackCv;
static AckState g_ack = {0};

static DeviceInfo g_devices[256];
static int g_devCount = 0;
static DeviceInfo g_activeUpdateDev;
static int g_activeUpdateDevValid = 0;
static DeviceInfo g_selectedTargetDev;
static int g_selectedTargetValid = 0;

typedef struct {
    uint8_t kind; /* 0=CAN, 1=RS ESP_UART, 2=ESP_CMD */
    uint32_t can_id;
    uint8_t data[8];
    uint8_t bus_label;
    uint8_t rs_addr;
    uint8_t rs_cmd;
    uint8_t rs_flags;
    uint8_t rs_payload_len;
    uint8_t rs_payload[16];
} PostedPacket;

static uint8_t g_rsTxSeq = 1;
static uint8_t g_espUartSeq = 0;
static uint16_t g_espCmdSeq = 0;
static volatile LONG g_panelBootSeen = 0;
static volatile LONG g_panelEnterAcked = 0;
static volatile LONG g_panelTransmitSeen = 0;
static volatile LONG g_panelTransmitOk = 0;
static volatile uint8_t g_panelWaitAddr = 0;

static void AppendLog(const wchar_t *msg) {
    if (!g_hLog) return;
    int len = GetWindowTextLengthW(g_hLog);
    SendMessageW(g_hLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)msg);
    SendMessageW(g_hLog, EM_SCROLLCARET, 0, 0);
}

static void Logf(const wchar_t *fmt, ...) {
    wchar_t buf[1024];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf(buf, 1023, fmt, args);
    va_end(args);
    buf[1023] = L'\0';
    AppendLog(buf);
}

static uint16_t BsuChecksum(const uint8_t *data, size_t sz) {
    uint32_t sum = 0;
    for (size_t i = 0; i < sz; i++) sum += data[i];
    return (uint16_t)(sum & 0xFFFFu);
}

static uint32_t BuildCanId(uint8_t d_type, uint8_t h_adr, uint8_t l_adr, uint8_t zone, uint8_t dir) {
    return ((uint32_t)(zone & 0x7F)) |
           ((uint32_t)(l_adr & 0x3F) << 7) |
           ((uint32_t)h_adr << 13) |
           ((uint32_t)(d_type & 0x7F) << 21) |
           ((uint32_t)(dir & 1) << 28);
}

static CanIdFields ParseCanId(uint32_t can_id) {
    CanIdFields f;
    f.dir = (uint8_t)((can_id >> 28) & 1);
    f.d_type = (uint8_t)((can_id >> 21) & 0x7F);
    f.h_adr = (uint8_t)((can_id >> 13) & 0xFF);
    f.l_adr = (uint8_t)((can_id >> 7) & 0x3F);
    f.zone = (uint8_t)(can_id & 0x7F);
    return f;
}

static int SerialWrite(const uint8_t *buf, DWORD sz);

static void HandleAckFastPath(uint32_t can_id, const uint8_t data[8]) {
    CanIdFields f = ParseCanId(can_id);
    if (f.dir != 1) return;
    if (data[0] != CMD_SET_UPDATE_WORD) return;

    uint32_t idx = ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
    uint32_t word = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) |
                    ((uint32_t)data[6] << 8) | data[7];

    EnterCriticalSection(&g_ackCs);
    if (g_ack.active) {
        for (uint32_t s = 0; s < g_ack.batch_len; s++) {
            if (!g_ack.acked[s] &&
                idx == g_ack.expect_idx[s] &&
                word == g_ack.expect_word[s]) {
                g_ack.acked[s] = 1;
                g_ack.acked_count++;
                WakeAllConditionVariable(&g_ackCv);
                break;
            }
        }
    }
    LeaveCriticalSection(&g_ackCs);
}

static void HandleAckRsFastPath(const uint8_t *payload, uint16_t payload_len) {
    uint32_t idx;
    uint32_t word;
    if (!payload || payload_len < 7u) return;
    idx = ((uint32_t)payload[0] << 16) | ((uint32_t)payload[1] << 8) | payload[2];
    word = ((uint32_t)payload[3] << 24) | ((uint32_t)payload[4] << 16) |
           ((uint32_t)payload[5] << 8) | payload[6];

    EnterCriticalSection(&g_ackCs);
    if (g_ack.active) {
        for (uint32_t s = 0; s < g_ack.batch_len; s++) {
            if (!g_ack.acked[s] &&
                idx == g_ack.expect_idx[s] &&
                word == g_ack.expect_word[s]) {
                g_ack.acked[s] = 1;
                g_ack.acked_count++;
                WakeAllConditionVariable(&g_ackCv);
                break;
            }
        }
    }
    LeaveCriticalSection(&g_ackCs);
}

static void HandleAckEspCmdFastPath(const uint8_t *payload, uint16_t payload_len) {
    uint32_t idx;
    uint32_t word;
    if (!payload || payload_len < 8u) return;
    if (payload[0] != CMD_SET_UPDATE_WORD) return;
    idx = ((uint32_t)payload[1] << 16) | ((uint32_t)payload[2] << 8) | payload[3];
    word = ((uint32_t)payload[4] << 24) | ((uint32_t)payload[5] << 16) |
           ((uint32_t)payload[6] << 8) | payload[7];

    EnterCriticalSection(&g_ackCs);
    if (g_ack.active) {
        for (uint32_t s = 0; s < g_ack.batch_len; s++) {
            if (!g_ack.acked[s] &&
                idx == g_ack.expect_idx[s] &&
                word == g_ack.expect_word[s]) {
                g_ack.acked[s] = 1;
                g_ack.acked_count++;
                WakeAllConditionVariable(&g_ackCv);
                break;
            }
        }
    }
    LeaveCriticalSection(&g_ackCs);
}

static uint16_t RsChecksum16(const uint8_t *data, uint16_t len) {
    uint32_t sum = 0;
    uint16_t i;
    if (!data) return 0;
    for (i = 0; i < len; i++) sum += data[i];
    return (uint16_t)(sum & 0xFFFFu);
}

static uint16_t RsFrameEncode(uint8_t *dst, uint16_t dst_size,
                              uint8_t addr, uint8_t seq, uint8_t flags, uint8_t cmd,
                              const uint8_t *payload, uint16_t payload_len) {
    uint16_t len_field = (uint16_t)(4u + payload_len);
    uint16_t total = (uint16_t)(2u + 1u + len_field + 2u);
    uint16_t crc;
    if (!dst || payload_len > 251u || dst_size < total) return 0;
    dst[0] = RS_BUS_PREAMBLE_0;
    dst[1] = RS_BUS_PREAMBLE_1;
    dst[2] = (uint8_t)len_field;
    dst[3] = addr;
    dst[4] = seq;
    dst[5] = flags;
    dst[6] = cmd;
    if (payload_len && payload) memcpy(&dst[7], payload, payload_len);
    crc = RsChecksum16(&dst[3], len_field);
    dst[7u + payload_len] = (uint8_t)(crc & 0xFFu);
    dst[8u + payload_len] = (uint8_t)(crc >> 8);
    return total;
}

static int RsFrameDecode(const uint8_t *src, uint16_t src_size,
                         uint8_t *addr, uint8_t *seq, uint8_t *flags, uint8_t *cmd,
                         const uint8_t **payload, uint16_t *payload_len) {
    uint16_t len_field;
    uint16_t total;
    uint16_t rx_crc;
    if (!src || src_size < 9u) return 0;
    if (src[0] != RS_BUS_PREAMBLE_0 || src[1] != RS_BUS_PREAMBLE_1) return 0;
    len_field = src[2];
    if (len_field < 4u) return 0;
    total = (uint16_t)(2u + 1u + len_field + 2u);
    if (src_size < total) return 0;
    rx_crc = (uint16_t)src[3u + len_field] | ((uint16_t)src[4u + len_field] << 8);
    if (rx_crc != RsChecksum16(&src[3], len_field)) return 0;
    if (addr) *addr = src[3];
    if (seq) *seq = src[4];
    if (flags) *flags = src[5];
    if (cmd) *cmd = src[6];
    if (payload) *payload = (len_field > 4u) ? &src[7] : NULL;
    if (payload_len) *payload_len = (uint16_t)(len_field - 4u);
    return 1;
}

static int SendBsuEspUart(const uint8_t *rs_frame, uint16_t rs_len) {
    uint8_t pkt[BSU_PKT_MAX_SIZE];
    uint16_t pkt_size;
    uint16_t pos;
    uint16_t crc;
    uint16_t seq;
    if (!rs_frame || rs_len == 0u || rs_len > ESP_UART_BODY_MAX) return 0;
    pkt_size = (uint16_t)(8u + rs_len + 2u);
    if (pkt_size > BSU_PKT_MAX_SIZE) return 0;
    seq = g_espUartSeq++;
    pos = 0;
    pkt[pos++] = BSU_PREAMBLE0;
    pkt[pos++] = BSU_PREAMBLE1;
    pkt[pos++] = (uint8_t)(pkt_size & 0xFF);
    pkt[pos++] = (uint8_t)(pkt_size >> 8);
    pkt[pos++] = (uint8_t)(BSU_PKT_TYPE_ESP_UART & 0xFF);
    pkt[pos++] = (uint8_t)(BSU_PKT_TYPE_ESP_UART >> 8);
    pkt[pos++] = (uint8_t)(seq & 0xFF);
    pkt[pos++] = (uint8_t)(seq >> 8);
    memcpy(&pkt[pos], rs_frame, rs_len);
    pos = (uint16_t)(pos + rs_len);
    crc = BsuChecksum(pkt, pos);
    pkt[pos++] = (uint8_t)(crc & 0xFF);
    pkt[pos++] = (uint8_t)(crc >> 8);
    return SerialWrite(pkt, pos);
}

static int SendBsuEspCmd(const uint8_t *payload, uint16_t payload_len) {
    uint8_t pkt[BSU_PKT_MAX_SIZE];
    uint16_t pkt_size;
    uint16_t pos;
    uint16_t crc;
    uint16_t seq;
    if (!payload || payload_len == 0u || payload_len > ESP_UART_BODY_MAX) return 0;
    pkt_size = (uint16_t)(8u + payload_len + 2u);
    if (pkt_size > BSU_PKT_MAX_SIZE) return 0;
    seq = g_espCmdSeq++;
    pos = 0;
    pkt[pos++] = BSU_PREAMBLE0;
    pkt[pos++] = BSU_PREAMBLE1;
    pkt[pos++] = (uint8_t)(pkt_size & 0xFF);
    pkt[pos++] = (uint8_t)(pkt_size >> 8);
    pkt[pos++] = (uint8_t)(BSU_PKT_TYPE_ESP_CMD & 0xFF);
    pkt[pos++] = (uint8_t)(BSU_PKT_TYPE_ESP_CMD >> 8);
    pkt[pos++] = (uint8_t)(seq & 0xFF);
    pkt[pos++] = (uint8_t)(seq >> 8);
    memcpy(&pkt[pos], payload, payload_len);
    pos = (uint16_t)(pos + payload_len);
    crc = BsuChecksum(pkt, pos);
    pkt[pos++] = (uint8_t)(crc & 0xFF);
    pkt[pos++] = (uint8_t)(crc >> 8);
    return SerialWrite(pkt, pos);
}

static int SendPanelRsCmd(uint8_t addr, uint8_t cmd, const uint8_t *payload, uint16_t payload_len) {
    uint8_t frame[ESP_UART_BODY_MAX];
    uint16_t n = RsFrameEncode(frame, (uint16_t)sizeof(frame), addr, g_rsTxSeq++, 0u, cmd,
                               payload, payload_len);
    if (n == 0u) return 0;
    return SendBsuEspUart(frame, n);
}

static int IsPanelDev(const DeviceInfo *d) {
    return d && d->d_type == DEVICE_PANEL_TYPE;
}

static int IsEspDev(const DeviceInfo *d) {
    return d && d->d_type == DEVICE_ESP32_TYPE;
}

static DWORD g_lastIoError = 0;

static int SerialWrite(const uint8_t *buf, DWORD sz) {
    g_lastIoError = 0;
    if (g_useTcp) {
        if (g_sock == INVALID_SOCKET) return 0;
        EnterCriticalSection(&g_ioCs);
        DWORD done = 0;
        DWORD stall = 0;
        while (done < sz) {
            int n = send(g_sock, (const char *)(buf + done), (int)(sz - done), 0);
            if (n == SOCKET_ERROR) {
                int err = WSAGetLastError();
                if (err == WSAEWOULDBLOCK) {
                    if (++stall > 2000) {
                        g_lastIoError = (DWORD)err;
                        LeaveCriticalSection(&g_ioCs);
                        return 0;
                    }
                    Sleep(1);
                    continue;
                }
                g_lastIoError = (DWORD)err;
                LeaveCriticalSection(&g_ioCs);
                return 0;
            }
            if (n <= 0) {
                g_lastIoError = (DWORD)WSAGetLastError();
                LeaveCriticalSection(&g_ioCs);
                return 0;
            }
            done += (DWORD)n;
            stall = 0;
        }
        LeaveCriticalSection(&g_ioCs);
        return 1;
    }
    if (g_hSerial == INVALID_HANDLE_VALUE) return 0;
    EnterCriticalSection(&g_ioCs);
    DWORD done = 0;
    DWORD stall = 0;
    while (done < sz) {
        DWORD wr = 0;
        DWORD comm_err = 0;
        COMSTAT st;
        ClearCommError(g_hSerial, &comm_err, &st);
        BOOL ok = WriteFile(g_hSerial, buf + done, sz - done, &wr, NULL);
        if (!ok) {
            g_lastIoError = GetLastError();
            ClearCommError(g_hSerial, &comm_err, &st);
            if (++stall > 40) {
                LeaveCriticalSection(&g_ioCs);
                return 0;
            }
            Sleep(10);
            continue;
        }
        if (wr == 0) {
            g_lastIoError = GetLastError();
            if (++stall > 80) {
                LeaveCriticalSection(&g_ioCs);
                return 0;
            }
            Sleep(5);
            continue;
        }
        done += wr;
        stall = 0;
    }
    LeaveCriticalSection(&g_ioCs);
    return 1;
}

static void BuildBsuCanPacket(uint8_t *pkt, uint32_t can_id, const uint8_t data[8], uint8_t bus_type) {
    pkt[0] = BSU_PREAMBLE0;
    pkt[1] = BSU_PREAMBLE1;
    pkt[2] = (uint8_t)(BSU_PKT_SIZE_CAN & 0xFF);
    pkt[3] = (uint8_t)(BSU_PKT_SIZE_CAN >> 8);
    pkt[4] = bus_type;
    pkt[5] = 0;
    pkt[6] = 0;
    pkt[7] = 0;
    pkt[8] = (uint8_t)(can_id & 0xFF);
    pkt[9] = (uint8_t)((can_id >> 8) & 0xFF);
    pkt[10] = (uint8_t)((can_id >> 16) & 0xFF);
    pkt[11] = (uint8_t)((can_id >> 24) & 0xFF);
    memcpy(&pkt[12], data, 8);
    uint16_t crc = BsuChecksum(pkt, 20);
    pkt[20] = (uint8_t)(crc & 0xFF);
    pkt[21] = (uint8_t)(crc >> 8);
}

static int SendBsuCanPacket(uint32_t can_id, const uint8_t data[8], uint8_t bus_type) {
    uint8_t pkt[BSU_PKT_SIZE_CAN];
    BuildBsuCanPacket(pkt, can_id, data, bus_type);
    return SerialWrite(pkt, sizeof(pkt));
}

static void ClearDeviceList(void) {
    g_devCount = 0;
    g_selectedTargetValid = 0;
    if (g_hDevices) {
        ListView_DeleteAllItems(g_hDevices);
    }
}

static int ComPortNumFromName(const wchar_t *name) {
    const wchar_t *p = name;
    if (!p || !p[0]) return -1;
    if ((p[0] == L'C' || p[0] == L'c') &&
        (p[1] == L'O' || p[1] == L'o') &&
        (p[2] == L'M' || p[2] == L'm')) {
        p += 3;
    }
    wchar_t *end = NULL;
    long n = wcstol(p, &end, 10);
    if (!end || end == p || n < 0 || n > 9999) return -1;
    return (int)n;
}

static void RefreshPorts(void) {
    typedef struct {
        wchar_t name[32];
        int num;
    } ComPortEntry;

    ComPortEntry ports[256];
    int count = 0;

    SendMessageW(g_hPort, CB_RESETCONTENT, 0, 0);

    HKEY hKey = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM",
                      0, KEY_READ | KEY_WOW64_64KEY, &hKey) != ERROR_SUCCESS) {
        RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM",
                      0, KEY_READ, &hKey);
    }
    if (hKey) {
        DWORD index = 0;
        wchar_t valueName[256];
        wchar_t portName[32];
        DWORD valueNameLen, portNameBytes, type;
        while (count < 256) {
            valueNameLen = (DWORD)(sizeof(valueName) / sizeof(valueName[0]));
            portNameBytes = sizeof(portName);
            if (RegEnumValueW(hKey, index++, valueName, &valueNameLen,
                              NULL, &type, (LPBYTE)portName, &portNameBytes) != ERROR_SUCCESS) {
                break;
            }
            int num = ComPortNumFromName(portName);
            if (num < 0) continue;
            wcsncpy(ports[count].name, portName, 31);
            ports[count].name[31] = L'\0';
            ports[count].num = num;
            count++;
        }
        RegCloseKey(hKey);
    }

    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (ports[j].num < ports[i].num) {
                ComPortEntry tmp = ports[i];
                ports[i] = ports[j];
                ports[j] = tmp;
            }
        }
    }

    for (int i = 0; i < count; i++) {
        SendMessageW(g_hPort, CB_ADDSTRING, 0, (LPARAM)ports[i].name);
    }
    if (count > 0) {
        SendMessageW(g_hPort, CB_SETCURSEL, 0, 0);
    }
}

static int IsWifiMode(void);

static void UpdateTransportUi(void) {
    int connected = InterlockedCompareExchange(&g_connected, 0, 0) ? 1 : 0;
    int wifi = IsWifiMode();
    EnableWindow(g_hPort, !connected && !wifi);
    EnableWindow(GetDlgItem(g_hwnd, IDC_BTN_REFRESH), !connected && !wifi);
    if (g_hMode) EnableWindow(g_hMode, !connected);
    if (g_hWifiHost) EnableWindow(g_hWifiHost, !connected && wifi);
    if (g_hWifiPort) EnableWindow(g_hWifiPort, !connected && wifi);
}

static void SetConnectedUi(int connected) {
    SetWindowTextW(g_hConnect, connected ? L"Отключить" : L"Подключить");
    UpdateTransportUi();
}

static DWORD WINAPI ReaderThreadProc(LPVOID arg) {
    (void)arg;
    enum {S_P0, S_P1, S_S0, S_S1, S_T0, S_T1, S_Q0, S_Q1, S_BODY, S_C0, S_C1} st = S_P0;
    uint16_t size = 0, type = 0, calc = 0, crc_rx = 0;
    uint8_t body[256];
    int body_need = 0, body_idx = 0;
    uint8_t c0 = 0;
    uint8_t rxbuf[512];
    uint8_t b = 0;
    DWORD rd = 0;

    while (!InterlockedCompareExchange(&g_readerStop, 0, 0)) {
        if (g_useTcp) {
            int n = recv(g_sock, (char *)rxbuf, (int)sizeof(rxbuf), 0);
            if (n == SOCKET_ERROR) {
                int err = WSAGetLastError();
                if (err == WSAEWOULDBLOCK || err == WSAETIMEDOUT || err == WSAEINTR) {
                    Sleep(InterlockedCompareExchange(&g_updateRunning, 0, 0) ? 0 : 1);
                    continue;
                }
                break;
            }
            if (n <= 0) {
                Sleep(InterlockedCompareExchange(&g_updateRunning, 0, 0) ? 0 : 1);
                continue;
            }
            rd = (DWORD)n;
        } else {
            if (!ReadFile(g_hSerial, rxbuf, sizeof(rxbuf), &rd, NULL) || rd == 0) {
                /* Во время обновления опрашиваем COM без паузы — ACK не должны залипать в буфере. */
                Sleep(InterlockedCompareExchange(&g_updateRunning, 0, 0) ? 0 : 1);
                continue;
            }
        }
        for (DWORD i = 0; i < rd; i++) {
            b = rxbuf[i];
            switch (st) {
                case S_P0: st = (b == BSU_PREAMBLE0) ? S_P1 : S_P0; break;
                case S_P1: if (b == BSU_PREAMBLE1) { calc = BSU_PREAMBLE0 + BSU_PREAMBLE1; st = S_S0; } else st = S_P0; break;
                case S_S0: size = b; calc += b; st = S_S1; break;
                case S_S1: size |= (uint16_t)b << 8; calc += b; st = S_T0; break;
                case S_T0: type = b; calc += b; st = S_T1; break;
                case S_T1: type |= (uint16_t)b << 8; calc += b; st = S_Q0; break;
                case S_Q0: calc += b; st = S_Q1; break;
                case S_Q1:
                    calc += b;
                    body_need = (int)size - 8 - 2;
                    body_idx = 0;
                    /* Любой валидный BSU-кадр дочитываем целиком. Иначе ACTIVITY (тип 2)
                     * и LOG (типы 16..18) сбивают синхронизацию — CAN ППКУ пропадает из потока. */
                    if (size < 10u || size > BSU_PKT_MAX_SIZE ||
                        body_need < 0 || body_need > (int)sizeof(body)) st = S_P0;
                    else if (body_need == 0) st = S_C0;
                    else st = S_BODY;
                    break;
                case S_BODY:
                    if (body_idx < (int)sizeof(body)) body[body_idx] = b;
                    body_idx++;
                    calc += b;
                    if (body_idx >= body_need) st = S_C0;
                    break;
                case S_C0: c0 = b; st = S_C1; break;
                case S_C1:
                    crc_rx = (uint16_t)c0 | ((uint16_t)b << 8);
                    if (((uint16_t)calc) == crc_rx) {
                        if ((type == 0 || type == 1 || type == BSU_PKT_TYPE_ESP_CAN) &&
                            body_need == 12) {
                            uint32_t can_id = (uint32_t)body[0] | ((uint32_t)body[1] << 8) |
                                              ((uint32_t)body[2] << 16) | ((uint32_t)body[3] << 24);
                            HandleAckFastPath(can_id, &body[4]);
                            CanIdFields f = ParseCanId(can_id);
                            int is_ack_packet = (f.dir == 1 && body[4] == CMD_SET_UPDATE_WORD);
                            if (!InterlockedCompareExchange(&g_updateRunning, 0, 0) || !is_ack_packet) {
                                PostedPacket *pp = (PostedPacket *)malloc(sizeof(PostedPacket));
                                if (pp) {
                                    memset(pp, 0, sizeof(*pp));
                                    pp->kind = 0;
                                    pp->can_id = can_id;
                                    memcpy(pp->data, &body[4], 8);
                                    pp->bus_label = (uint8_t)type;
                                    PostMessageW(g_hwnd, WM_APP_PACKET, 0, (LPARAM)pp);
                                }
                            }
                        } else if (type == BSU_PKT_TYPE_ESP_UART) {
                            uint8_t rs_addr = 0, rs_seq = 0, rs_flags = 0, rs_cmd = 0;
                            const uint8_t *rs_pl = NULL;
                            uint16_t rs_plen = 0;
                            if (RsFrameDecode(body, (uint16_t)body_need, &rs_addr, &rs_seq,
                                              &rs_flags, &rs_cmd, &rs_pl, &rs_plen)) {
                                int is_dir = (rs_flags & RS_BUS_FLAG_DIR) != 0;
                                if (is_dir && rs_cmd == CMD_SET_UPDATE_WORD) {
                                    HandleAckRsFastPath(rs_pl, rs_plen);
                                }
                                if (is_dir && rs_cmd == RS_PANEL_RSP_ACTIVITY &&
                                    rs_plen >= 10u && rs_pl &&
                                    rs_pl[0] == RS_BUS_DEV_TYPE_PANEL_BOOTLOADER &&
                                    rs_addr == g_panelWaitAddr) {
                                    InterlockedExchange(&g_panelBootSeen, 1);
                                }
                                if (is_dir && rs_cmd == RS_PANEL_RSP_ACK &&
                                    rs_addr == g_panelWaitAddr) {
                                    InterlockedExchange(&g_panelEnterAcked, 1);
                                }
                                if (is_dir && rs_cmd == CMD_UPDATE_TRANSMIT &&
                                    rs_addr == g_panelWaitAddr) {
                                    InterlockedExchange(&g_panelTransmitSeen, 1);
                                    if (rs_pl && rs_plen >= 1u && rs_pl[0] == 1u) {
                                        InterlockedExchange(&g_panelTransmitOk, 1);
                                    } else {
                                        InterlockedExchange(&g_panelTransmitOk, 0);
                                    }
                                }
                                int is_ack_packet = (is_dir && rs_cmd == CMD_SET_UPDATE_WORD);
                                if (!InterlockedCompareExchange(&g_updateRunning, 0, 0) || !is_ack_packet) {
                                    PostedPacket *pp = (PostedPacket *)malloc(sizeof(PostedPacket));
                                    if (pp) {
                                        memset(pp, 0, sizeof(*pp));
                                        pp->kind = 1;
                                        pp->bus_label = (uint8_t)type;
                                        pp->rs_addr = rs_addr;
                                        pp->rs_cmd = rs_cmd;
                                        pp->rs_flags = rs_flags;
                                        pp->rs_payload_len = (rs_plen > 16u) ? 16u : (uint8_t)rs_plen;
                                        if (rs_pl && pp->rs_payload_len)
                                            memcpy(pp->rs_payload, rs_pl, pp->rs_payload_len);
                                        PostMessageW(g_hwnd, WM_APP_PACKET, 0, (LPARAM)pp);
                                    }
                                }
                            }
                        } else if (type == BSU_PKT_TYPE_ESP_CMD && body_need >= 1) {
                            uint8_t cmd = body[0];
                            int is_ack_packet = (cmd == CMD_SET_UPDATE_WORD && body_need >= 8);
                            if (is_ack_packet)
                                HandleAckEspCmdFastPath(body, (uint16_t)body_need);
                            if (!InterlockedCompareExchange(&g_updateRunning, 0, 0) || !is_ack_packet) {
                                PostedPacket *pp = (PostedPacket *)malloc(sizeof(PostedPacket));
                                if (pp) {
                                    memset(pp, 0, sizeof(*pp));
                                    pp->kind = 2;
                                    pp->bus_label = (uint8_t)type;
                                    if (body_need > 0) {
                                        uint16_t n = (body_need > 8) ? 8 : (uint16_t)body_need;
                                        memcpy(pp->data, body, n);
                                    }
                                    PostMessageW(g_hwnd, WM_APP_PACKET, 0, (LPARAM)pp);
                                }
                            }
                        }
                    }
                    st = S_P0;
                    break;
            }
        }
    }
    return 0;
}

static int IsWifiMode(void) {
    if (!g_hMode) return 0;
    return (int)SendMessageW(g_hMode, CB_GETCURSEL, 0, 0) == 1;
}

static int ConnectTcp(void) {
    wchar_t host_w[64] = {0};
    wchar_t port_w[16] = {0};
    char host_a[64] = {0};
    GetWindowTextW(g_hWifiHost, host_w, 63);
    GetWindowTextW(g_hWifiPort, port_w, 15);
    if (host_w[0] == 0) return 0;
    WideCharToMultiByte(CP_UTF8, 0, host_w, -1, host_a, (int)sizeof(host_a), NULL, NULL);
    int port = _wtoi(port_w);
    if (port <= 0 || port > 65535) port = 23;

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;

    BOOL nd = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nd, sizeof(nd));
    BOOL ka = TRUE;
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (const char *)&ka, sizeof(ka));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    if (inet_pton(AF_INET, host_a, &addr.sin_addr) != 1) {
        closesocket(s);
        return 0;
    }

    DWORD timeout_ms = 2000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout_ms, sizeof(timeout_ms));

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(s);
        return 0;
    }

    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);

    g_sock = s;
    g_useTcp = 1;
    InterlockedExchange(&g_readerStop, 0);
    g_hReaderThread = CreateThread(NULL, 0, ReaderThreadProc, NULL, 0, NULL);
    if (!g_hReaderThread) {
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        g_useTcp = 0;
        return 0;
    }
    SetThreadPriority(g_hReaderThread, THREAD_PRIORITY_ABOVE_NORMAL);
    InterlockedExchange(&g_connected, 1);
    ClearDeviceList();
    return 1;
}

static int ConnectSerial(void) {
    wchar_t port[64];
    GetWindowTextW(g_hPort, port, 63);
    wchar_t path[80];
    wsprintfW(path, L"\\\\.\\%s", port);

    g_hSerial = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (g_hSerial == INVALID_HANDLE_VALUE) return 0;
    g_useTcp = 0;

    DCB dcb = {0};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(g_hSerial, &dcb)) return 0;
    dcb.BaudRate = 1000000;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fTXContinueOnXoff = TRUE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fErrorChar = FALSE;
    dcb.fNull = FALSE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fAbortOnError = FALSE;
    if (!SetCommState(g_hSerial, &dcb)) return 0;
    SetupComm(g_hSerial, 65536, 65536);

    COMMTIMEOUTS to = {0};
    /* Truly nonblocking read: ReadFile возвращает сразу, если данных нет. */
    to.ReadIntervalTimeout = MAXDWORD;
    to.ReadTotalTimeoutConstant = 0;
    to.ReadTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant = 2000;
    to.WriteTotalTimeoutMultiplier = 5;
    SetCommTimeouts(g_hSerial, &to);
    PurgeComm(g_hSerial, PURGE_RXCLEAR | PURGE_TXCLEAR);

    InterlockedExchange(&g_readerStop, 0);
    g_hReaderThread = CreateThread(NULL, 0, ReaderThreadProc, NULL, 0, NULL);
    if (!g_hReaderThread) return 0;
    SetThreadPriority(g_hReaderThread, THREAD_PRIORITY_ABOVE_NORMAL);
    InterlockedExchange(&g_connected, 1);
    ClearDeviceList();
    return 1;
}

static void DisconnectSerial(void) {
    InterlockedExchange(&g_readerStop, 1);
    if (g_sock != INVALID_SOCKET) {
        shutdown(g_sock, SD_BOTH);
    }
    if (g_hReaderThread) {
        WaitForSingleObject(g_hReaderThread, 1000);
        CloseHandle(g_hReaderThread);
        g_hReaderThread = NULL;
    }
    if (g_hSerial != INVALID_HANDLE_VALUE) {
        CloseHandle(g_hSerial);
        g_hSerial = INVALID_HANDLE_VALUE;
    }
    if (g_sock != INVALID_SOCKET) {
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
    }
    g_useTcp = 0;
    InterlockedExchange(&g_connected, 0);
}

static int FindOrAddDevice(DeviceInfo d) {
    for (int i = 0; i < g_devCount; i++) {
        if (g_devices[i].d_type == d.d_type && g_devices[i].h_adr == d.h_adr &&
            g_devices[i].l_adr == d.l_adr && g_devices[i].zone == d.zone) return i;
    }
    if (g_devCount >= 256) return -1;
    g_devices[g_devCount] = d;
    return g_devCount++;
}

static const wchar_t* DeviceTypeNameW(uint8_t d_type) {
    switch (d_type) {
        case 10: return L"ППКУ";
        case 11: return L"ESP32";
        case 13: return L"МКУ_IGN";
        case 14: return L"МКУ_TC";
        case 20: return L"МКУ_K1";
        case 21: return L"МКУ_K2";
        case 22: return L"МКУ_K3";
        case 23: return L"МКУ_KR";
        case 30: return L"Панель";
        default: return L"устройство";
    }
}

static void VersionStrToWide(const char *src, wchar_t *dst, size_t dst_chars) {
    if (!dst || dst_chars == 0) return;
    dst[0] = L'\0';
    if (!src || src[0] == '\0') return;
    if (MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, (int)dst_chars) > 0)
        return;
    if (MultiByteToWideChar(CP_ACP, 0, src, -1, dst, (int)dst_chars) <= 0)
        dst[0] = L'\0';
}

static void RefreshDeviceListRow(int idx) {
    if (idx < 0 || idx >= g_devCount) return;
    wchar_t text[128];
    LVITEMW it = {0};
    it.mask = LVIF_TEXT;
    it.iItem = idx;

    const wchar_t *name = DeviceTypeNameW(g_devices[idx].d_type);
    wsprintfW(text, L"%s (%u)", name, g_devices[idx].d_type); it.iSubItem = 0; it.pszText = text; ListView_SetItem(g_hDevices, &it);
    wsprintfW(text, L"%u", g_devices[idx].h_adr); it.iSubItem = 1; it.pszText = text; ListView_SetItem(g_hDevices, &it);
    wsprintfW(text, L"%u", g_devices[idx].l_adr); it.iSubItem = 2; it.pszText = text; ListView_SetItem(g_hDevices, &it);
    wsprintfW(text, L"%u", g_devices[idx].zone);  it.iSubItem = 3; it.pszText = text; ListView_SetItem(g_hDevices, &it);
    if (g_devices[idx].version_valid) {
        if (g_devices[idx].version_str[0] != '\0')
            VersionStrToWide(g_devices[idx].version_str, text, sizeof(text) / sizeof(text[0]));
        else
            wsprintfW(text, L"%u", g_devices[idx].version);
    }
    else lstrcpyW(text, L"...");
    it.iSubItem = 4; it.pszText = text; ListView_SetItem(g_hDevices, &it);
}

static void AddDeviceToList(int idx) {
    LVITEMW it = {0};
    wchar_t text[64];
    it.mask = LVIF_TEXT;
    it.iItem = idx;
    wsprintfW(text, L"%u", g_devices[idx].d_type);
    it.iSubItem = 0;
    it.pszText = text;
    ListView_InsertItem(g_hDevices, &it);
    RefreshDeviceListRow(idx);
}

static void ResetDeviceVersionAssembly(DeviceInfo *d) {
    if (!d) return;
    d->version_valid = 0;
    d->version = 0;
    d->version_str[0] = '\0';
    d->version_pkt_next = 0;
}

static uint32_t ParseLastUint(const char *s) {
    uint32_t last = 0;
    int found = 0;
    if (!s) return 0;
    while (*s) {
        if (*s >= '0' && *s <= '9') {
            uint32_t v = 0;
            while (*s >= '0' && *s <= '9') {
                v = v * 10u + (uint32_t)(*s - '0');
                s++;
            }
            last = v;
            found = 1;
        } else {
            s++;
        }
    }
    return found ? last : 0;
}

static void ParseVersionString(DeviceInfo *d) {
    uint32_t fw = 0;
    if (!d) return;
    /* МКУ: "fw=123". ППКУ: "БСУ 4 версия аппаратной части 5" — берём последнее число. */
    if (sscanf(d->version_str, "fw=%u", &fw) == 1) {
        d->version = fw;
    } else {
        d->version = ParseLastUint(d->version_str);
    }
    d->version_valid = 1;
}

static int VersionPayloadEmpty(const uint8_t *data) {
    int i;
    if (!data) return 1;
    for (i = 2; i < 8; i++) {
        if (data[i] != 0) return 0;
    }
    return 1;
}

static void HandleVersionPacket(DeviceInfo *d, const uint8_t *data) {
    uint8_t pkt;
    size_t len;
    int i;

    if (!d || !data) return;
    pkt = data[1];
    /* Эхо запроса 159 (пустое тело) — не начало строки версии. */
    if (pkt == 0 && VersionPayloadEmpty(data))
        return;
    if (pkt == 0) {
        ResetDeviceVersionAssembly(d);
    } else if (pkt != d->version_pkt_next) {
        /* Старые фрагменты после повторного 159 не должны сбрасывать сборку. */
        return;
    }

    len = strlen(d->version_str);
    for (i = 2; i < 8; i++) {
        if (data[i] == 0) {
            ParseVersionString(d);
            d->version_pkt_next = 0;
            return;
        }
        if (len + 1 >= sizeof(d->version_str)) {
            ParseVersionString(d);
            d->version_pkt_next = 0;
            return;
        }
        d->version_str[len++] = (char)data[i];
        d->version_str[len] = '\0';
    }
    d->version_pkt_next = (uint8_t)(pkt + 1u);
}

static int IsUpdatableType(uint8_t d_type) {
    return (d_type == DEVICE_PPKY_TYPE ||
            d_type == DEVICE_ESP32_TYPE ||
            d_type == DEVICE_PANEL_TYPE ||
            d_type == 13 || d_type == 14 ||
            d_type == 20 || d_type == 21 || d_type == 22 || d_type == 23);
}

static void RequestDeviceVersion(const DeviceInfo *d) {
    int i;
    if (!d) return;
    for (i = 0; i < g_devCount; i++) {
        if (g_devices[i].d_type == d->d_type &&
            g_devices[i].h_adr == d->h_adr &&
            g_devices[i].l_adr == d->l_adr &&
            g_devices[i].zone == d->zone) {
            g_devices[i].version_req_sent = 1;
            g_devices[i].version_req_ms = GetTickCount();
            break;
        }
    }
    if (IsPanelDev(d)) {
        if (!SendPanelRsCmd(d->l_adr, CMD_GET_VERSION, NULL, 0))
            Logf(L"Не удалось отправить 159 на панель RS=%u (err=%u)\r\n", d->l_adr, g_lastIoError);
        return;
    }
    if (IsEspDev(d)) {
        uint8_t data[8] = { CMD_GET_VERSION, 0, 0, 0, 0, 0, 0, 0 };
        if (!SendBsuEspCmd(data, 8))
            Logf(L"Не удалось отправить 159 на ESP32 (err=%u)\r\n", g_lastIoError);
        return;
    }
    uint32_t can_id_req = BuildCanId(d->d_type, d->h_adr, d->l_adr, d->zone, 0);
    uint8_t data[8] = { CMD_GET_VERSION, 0, 0, 0, 0, 0, 0, 0 };
    if (!SendBsuCanPacket(can_id_req, data, BSU_PKT_TYPE_CAN1))
        Logf(L"Не удалось отправить 159 (%s h=%u l=%u, err=%u)\r\n",
             DeviceTypeNameW(d->d_type), d->h_adr, d->l_adr, g_lastIoError);
}

static void InvalidateDeviceVersionCache(const DeviceInfo *d) {
    if (!d) return;
    for (int i = 0; i < g_devCount; i++) {
        if (g_devices[i].d_type == d->d_type &&
            g_devices[i].h_adr == d->h_adr &&
            g_devices[i].l_adr == d->l_adr &&
            g_devices[i].zone == d->zone) {
            ResetDeviceVersionAssembly(&g_devices[i]);
            g_devices[i].version_req_sent = 0;
            RefreshDeviceListRow(i);
            return;
        }
    }
}

typedef struct {
    DeviceInfo snapshot[256];
    int count;
    int accepted;
    DeviceInfo selected;
    HWND hList;
} TargetSelectCtx;

static LRESULT CALLBACK TargetSelectWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    TargetSelectCtx* ctx = (TargetSelectCtx*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
            ctx = (TargetSelectCtx*)cs->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)ctx);

            CreateWindowW(L"STATIC", L"Выберите устройство для обновления:", WS_CHILD | WS_VISIBLE,
                          10, 10, 360, 18, hwnd, NULL, NULL, NULL);
            ctx->hList = CreateWindowW(WC_LISTBOXW, L"",
                                       WS_CHILD | WS_VISIBLE | WS_BORDER | LBS_NOTIFY | WS_VSCROLL,
                                       10, 32, 500, 220, hwnd, (HMENU)1001, NULL, NULL);
            CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                          330, 260, 85, 28, hwnd, (HMENU)IDOK, NULL, NULL);
            CreateWindowW(L"BUTTON", L"Отмена", WS_CHILD | WS_VISIBLE,
                          425, 260, 85, 28, hwnd, (HMENU)IDCANCEL, NULL, NULL);

            for (int i = 0; i < ctx->count; i++) {
                wchar_t row[256];
                const wchar_t* nm = DeviceTypeNameW(ctx->snapshot[i].d_type);
                if (ctx->snapshot[i].d_type == DEVICE_PANEL_TYPE) {
                    const wchar_t *mode = (ctx->snapshot[i].rs_dev_type == RS_BUS_DEV_TYPE_PANEL_BOOTLOADER)
                                          ? L"boot" : L"app";
                    if (ctx->snapshot[i].version_valid) {
                        wsprintfW(row, L"%s  RS=%u  %s  ver=%u",
                                  nm, ctx->snapshot[i].l_adr, mode, ctx->snapshot[i].version);
                    } else {
                        wsprintfW(row, L"%s  RS=%u  %s  ver=...",
                                  nm, ctx->snapshot[i].l_adr, mode);
                    }
                } else if (ctx->snapshot[i].version_valid) {
                    if (ctx->snapshot[i].version_str[0] != '\0') {
                        wchar_t ver_w[128];
                        VersionStrToWide(ctx->snapshot[i].version_str, ver_w,
                                         sizeof(ver_w) / sizeof(ver_w[0]));
                        wsprintfW(row, L"%s  h=%u  l=%u  z=%u  %s",
                                  nm, ctx->snapshot[i].h_adr, ctx->snapshot[i].l_adr,
                                  ctx->snapshot[i].zone, ver_w);
                    } else {
                        wsprintfW(row, L"%s  h=%u  l=%u  z=%u  ver=%u",
                                  nm, ctx->snapshot[i].h_adr, ctx->snapshot[i].l_adr,
                                  ctx->snapshot[i].zone, ctx->snapshot[i].version);
                    }
                } else {
                    wsprintfW(row, L"%s  h=%u  l=%u  z=%u  ver=...",
                              nm, ctx->snapshot[i].h_adr, ctx->snapshot[i].l_adr,
                              ctx->snapshot[i].zone);
                }
                int idx = (int)SendMessageW(ctx->hList, LB_ADDSTRING, 0, (LPARAM)row);
                SendMessageW(ctx->hList, LB_SETITEMDATA, idx, (LPARAM)i);
            }
            if (ctx->count > 0) {
                SendMessageW(ctx->hList, LB_SETCURSEL, 0, 0);
            }
            return 0;
        }
        case WM_COMMAND: {
            if (LOWORD(wp) == IDOK) {
                if (!ctx || !ctx->hList) break;
                int sel = (int)SendMessageW(ctx->hList, LB_GETCURSEL, 0, 0);
                if (sel == LB_ERR) {
                    MessageBoxW(hwnd, L"Выберите устройство.", L"Выбор цели", MB_ICONWARNING);
                    return 0;
                }
                int snapshot_idx = (int)SendMessageW(ctx->hList, LB_GETITEMDATA, sel, 0);
                if (snapshot_idx >= 0 && snapshot_idx < ctx->count) {
                    ctx->selected = ctx->snapshot[snapshot_idx];
                    ctx->accepted = 1;
                }
                DestroyWindow(hwnd);
                return 0;
            } else if (LOWORD(wp) == IDCANCEL) {
                DestroyWindow(hwnd);
                return 0;
            } else if (LOWORD(wp) == 1001 && HIWORD(wp) == LBN_DBLCLK) {
                SendMessageW(hwnd, WM_COMMAND, IDOK, 0);
                return 0;
            }
            break;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int SelectTargetDeviceDialog(HWND owner, DeviceInfo* out_dev) {
    if (!out_dev) return 0;
    if (g_devCount <= 0) {
        MessageBoxW(owner, L"Список устройств пуст. Дождитесь пакетов.", L"Выбор цели", MB_ICONWARNING);
        return 0;
    }

    TargetSelectCtx ctx;
    ZeroMemory(&ctx, sizeof(ctx));
    for (int i = 0; i < g_devCount && i < 256; i++) {
        if (!IsUpdatableType(g_devices[i].d_type)) continue;
        ctx.snapshot[ctx.count++] = g_devices[i];
    }
    if (ctx.count <= 0) {
        MessageBoxW(owner, L"В списке нет устройств для обновления.", L"Выбор цели", MB_ICONWARNING);
        return 0;
    }

    static int cls_registered = 0;
    if (!cls_registered) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc = TargetSelectWndProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"TargetSelectWndClass";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        if (!RegisterClassW(&wc)) {
            MessageBoxW(owner, L"Не удалось открыть окно выбора цели.", L"Updater", MB_ICONERROR);
            return 0;
        }
        cls_registered = 1;
    }

    EnableWindow(owner, FALSE);
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"TargetSelectWndClass", L"Выбор цели обновления",
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                               CW_USEDEFAULT, CW_USEDEFAULT, 540, 330,
                               owner, NULL, GetModuleHandleW(NULL), &ctx);
    if (!dlg) {
        EnableWindow(owner, TRUE);
        MessageBoxW(owner, L"Не удалось создать окно выбора цели.", L"Updater", MB_ICONERROR);
        return 0;
    }
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);

    MSG m;
    while (IsWindow(dlg) && GetMessageW(&m, NULL, 0, 0)) {
        if (!IsDialogMessageW(dlg, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);

    if (ctx.accepted) {
        *out_dev = ctx.selected;
        return 1;
    }
    return 0;
}

static void ForceReadVersions(void) {
    if (!InterlockedCompareExchange(&g_connected, 0, 0)) {
        MessageBoxW(g_hwnd, L"Сначала подключитесь.", L"Updater", MB_ICONWARNING);
        return;
    }
    int sent = 0;
    for (int i = 0; i < g_devCount; i++) {
        if (!IsUpdatableType(g_devices[i].d_type)) continue;
        ResetDeviceVersionAssembly(&g_devices[i]);
        g_devices[i].version_req_sent = 0;
        g_devices[i].version_retries = 0;
        RefreshDeviceListRow(i);
        RequestDeviceVersion(&g_devices[i]);
        sent++;
    }
    if (sent == 0) {
        Logf(L"Нет устройств для запроса версий.\r\n");
    } else {
        Logf(L"Принудительный запрос версий отправлен: %d устройств.\r\n", sent);
    }
}

static void EnsureEsp32Device(void) {
    DeviceInfo d = {0};
    int idx;
    d.d_type = DEVICE_ESP32_TYPE;
    d.last_seen_ms = GetTickCount();
    idx = FindOrAddDevice(d);
    if (idx < 0) return;
    g_devices[idx].last_seen_ms = d.last_seen_ms;
    if (idx >= ListView_GetItemCount(g_hDevices)) AddDeviceToList(idx);
    else RefreshDeviceListRow(idx);
    if (!g_devices[idx].version_valid && !g_devices[idx].version_req_sent)
        PostMessageW(g_hwnd, WM_APP_REQ_VERSION, (WPARAM)idx, 0);
}

static void RetryIncompleteVersions(void) {
    DWORD now;
    if (!InterlockedCompareExchange(&g_connected, 0, 0)) return;
    if (InterlockedCompareExchange(&g_updateRunning, 0, 0)) return;
    now = GetTickCount();
    for (int i = 0; i < g_devCount; i++) {
        DeviceInfo *d = &g_devices[i];
        if (!IsUpdatableType(d->d_type)) continue;
        if (d->version_valid) continue;
        if (!d->version_req_sent) continue;
        if (d->version_retries >= VERSION_RETRY_MAX) continue;
        /* Ещё собираем текущую серию пакетов. */
        if (d->version_pkt_next != 0 && (now - d->last_seen_ms) < 1000u) continue;
        if ((now - d->version_req_ms) < VERSION_RETRY_MS) continue;
        ResetDeviceVersionAssembly(d);
        d->version_retries++;
        RefreshDeviceListRow(i);
        RequestDeviceVersion(d);
    }
}

static void SelectUpdateTarget(void) {
    DeviceInfo d;
    if (SelectTargetDeviceDialog(g_hwnd, &d)) {
        g_selectedTargetDev = d;
        g_selectedTargetValid = 1;
        Logf(L"Цель обновления: %s h=%u l=%u z=%u\r\n",
             DeviceTypeNameW(d.d_type), d.h_adr, d.l_adr, d.zone);
        if (d.d_type == DEVICE_PANEL_TYPE) {
            Logf(L"  панель RS-addr=%u (%s)\r\n", d.l_adr,
                 (d.rs_dev_type == RS_BUS_DEV_TYPE_PANEL_BOOTLOADER) ? L"bootloader" : L"app");
        }
        if (d.d_type == DEVICE_ESP32_TYPE) {
            Logf(L"  ESP32: протокол ESP_CMD (тип 3), файл .bin IDF (не *_builder.bin).\r\n");
        }
    }
}

static uint32_t ReadFirmwareWord(const uint8_t *fw_buf, long fw_sz, uint32_t word_idx) {
    uint8_t b0 = 0xFF, b1 = 0xFF, b2 = 0xFF, b3 = 0xFF;
    uint32_t off = word_idx * 4u;
    if (off + 0 < (uint32_t)fw_sz) b0 = fw_buf[off + 0];
    if (off + 1 < (uint32_t)fw_sz) b1 = fw_buf[off + 1];
    if (off + 2 < (uint32_t)fw_sz) b2 = fw_buf[off + 2];
    if (off + 3 < (uint32_t)fw_sz) b3 = fw_buf[off + 3];
    return ((uint32_t)b3 << 24) | ((uint32_t)b2 << 16) | ((uint32_t)b1 << 8) | b0;
}

static void BuildUpdateWordData(uint8_t d[8], uint32_t word_idx, uint32_t word) {
    d[0] = (uint8_t)CMD_SET_UPDATE_WORD;
    d[1] = (uint8_t)((word_idx >> 16) & 0xFF);
    d[2] = (uint8_t)((word_idx >> 8) & 0xFF);
    d[3] = (uint8_t)(word_idx & 0xFF);
    d[4] = (uint8_t)((word >> 24) & 0xFF);
    d[5] = (uint8_t)((word >> 16) & 0xFF);
    d[6] = (uint8_t)((word >> 8) & 0xFF);
    d[7] = (uint8_t)(word & 0xFF);
}

static void BuildRsUpdateWordPayload(uint8_t d[7], uint32_t word_idx, uint32_t word) {
    d[0] = (uint8_t)((word_idx >> 16) & 0xFF);
    d[1] = (uint8_t)((word_idx >> 8) & 0xFF);
    d[2] = (uint8_t)(word_idx & 0xFF);
    d[3] = (uint8_t)((word >> 24) & 0xFF);
    d[4] = (uint8_t)((word >> 16) & 0xFF);
    d[5] = (uint8_t)((word >> 8) & 0xFF);
    d[6] = (uint8_t)(word & 0xFF);
}

static int SendUpdateWord(const DeviceInfo *dev, uint32_t can_id, uint32_t word_idx, uint32_t word) {
    if (IsPanelDev(dev)) {
        uint8_t pl[7];
        BuildRsUpdateWordPayload(pl, word_idx, word);
        return SendPanelRsCmd(dev->l_adr, CMD_SET_UPDATE_WORD, pl, 7u);
    }
    if (IsEspDev(dev)) {
        uint8_t payload[8];
        BuildUpdateWordData(payload, word_idx, word);
        return SendBsuEspCmd(payload, 8);
    }
    {
        uint8_t payload[8];
        BuildUpdateWordData(payload, word_idx, word);
        return SendBsuCanPacket(can_id, payload, BSU_PKT_TYPE_CAN1);
    }
}

static void AckBatchBeginLocked(uint32_t batch_start, uint32_t batch_len, const uint32_t *words) {
    g_ack.active = 1;
    g_ack.batch_start = batch_start;
    g_ack.batch_len = batch_len;
    g_ack.acked_count = 0;
    memset(g_ack.acked, 0, batch_len);
    for (uint32_t j = 0; j < batch_len; j++) {
        g_ack.expect_idx[j] = batch_start + j;
        g_ack.expect_word[j] = words[j];
    }
}

static uint32_t AckBatchWordBudgetMs(void) {
    return g_useTcp ? ACK_BATCH_WORD_BUDGET_WIFI : ACK_BATCH_WORD_BUDGET_USB;
}

static uint32_t AckBatchIdleGapMs(uint32_t missing) {
    int panel = (g_activeUpdateDevValid && g_activeUpdateDev.d_type == DEVICE_PANEL_TYPE);
    /* После хотя бы одного ACK: тишина = конец потока или потеря.
     * Для 1–2 хвостов ждём дольше — иначе ложный «Повтор … 1 из 64». */
    if (panel && g_useTcp) {
        if (missing > 0u && missing <= 2u) {
            return ACK_BATCH_IDLE_GAP_PANEL_TAIL_WIFI;
        }
        return ACK_BATCH_IDLE_GAP_PANEL_WIFI;
    }
    if (missing > 0u && missing <= 2u) {
        return g_useTcp ? ACK_BATCH_IDLE_GAP_TAIL_WIFI : ACK_BATCH_IDLE_GAP_TAIL_USB;
    }
    return g_useTcp ? ACK_BATCH_IDLE_GAP_WIFI : ACK_BATCH_IDLE_GAP_USB;
}

static uint32_t AckBatchHardTimeoutMs(uint32_t batch_start, uint32_t unacked) {
    int ppky = (g_activeUpdateDevValid && g_activeUpdateDev.d_type == DEVICE_PPKY_TYPE);
    int esp = (g_activeUpdateDevValid && g_activeUpdateDev.d_type == DEVICE_ESP32_TYPE);
    int panel = (g_activeUpdateDevValid && g_activeUpdateDev.d_type == DEVICE_PANEL_TYPE);
    uint32_t per = AckBatchWordBudgetMs();
    uint32_t t;

    if (unacked == 0u) {
        return ACK_BATCH_MIN_MS;
    }
    t = ACK_BATCH_MIN_MS + unacked * per;
    /* Слово 0 ППКУ/панели/ESP32: erase до ~35 с — только для первой пачки. */
    if (batch_start == 0u && (ppky || panel || esp)) {
        uint32_t rest = (unacked > 1u) ? ((unacked - 1u) * per) : 0u;
        uint32_t first = ACK_WAIT_MS_PPKY_FIRST + rest;
        if (first > t) {
            t = first;
        }
    } else if (batch_start == 0u && g_useTcp) {
        uint32_t first = 8000u + ((unacked > 1u) ? ((unacked - 1u) * per) : 0u);
        if (first > t) {
            t = first;
        }
    }
    return t;
}

/* Ждём ACK всей пачки. Если поток ACK оборвался (тишина idle_gap) —
 * выходим сразу и шлём только недостающие слова, без ожидания 64×400 мс. */
static int WaitAckBatch(uint32_t batch_start) {
    DWORD start = GetTickCount();
    DWORD last_progress = start;
    uint32_t last_count;
    uint32_t hard_tmo;
    int ok = 0;

    EnterCriticalSection(&g_ackCs);
    last_count = g_ack.acked_count;
    hard_tmo = AckBatchHardTimeoutMs(batch_start, g_ack.batch_len - g_ack.acked_count);

    while (g_ack.acked_count < g_ack.batch_len &&
           !InterlockedCompareExchange(&g_updateStop, 0, 0)) {
        DWORD now = GetTickCount();
        uint32_t missing = g_ack.batch_len - g_ack.acked_count;
        uint32_t idle_gap = AckBatchIdleGapMs(missing);

        if ((now - start) >= hard_tmo) {
            break;
        }
        /* Уже есть часть ACK, но новые не приходят — потеря кадра, не ждём потолок. */
        if (g_ack.acked_count > 0u && (now - last_progress) >= idle_gap) {
            break;
        }
        SleepConditionVariableCS(&g_ackCv, &g_ackCs, 2);
        now = GetTickCount();
        if (g_ack.acked_count > last_count) {
            last_count = g_ack.acked_count;
            last_progress = now;
        }
    }
    ok = (g_ack.acked_count >= g_ack.batch_len) ? 1 : 0;
    LeaveCriticalSection(&g_ackCs);
    return ok;
}

static int SendUnackedBatchWords(const DeviceInfo *dev, uint32_t can_id, uint32_t batch_start,
                                 const uint32_t *batch_words) {
    uint8_t already[MAX_BATCH_SIZE];
    uint32_t n;
    uint32_t j;
    EnterCriticalSection(&g_ackCs);
    n = g_ack.batch_len;
    memcpy(already, g_ack.acked, n);
    LeaveCriticalSection(&g_ackCs);

    for (j = 0; j < n; j++) {
        if (already[j]) {
            continue;
        }
        if (InterlockedCompareExchange(&g_updateStop, 0, 0)) {
            return 0;
        }
        if (!SendUpdateWord(dev, can_id, batch_start + j, batch_words[j])) {
            Logf(L"Ошибка отправки слова %u (err=%u)\r\n", batch_start + j, g_lastIoError);
            return 0;
        }
        /* Панель RS/WiFi: не заливать inject-очередь ППКУ и RX бутлоадера. */
        if (IsPanelDev(dev) && g_useTcp) {
            Sleep(PANEL_WORD_PACE_MS_WIFI);
        } else if (g_useTcp && ((j + 1u) % 8u) == 0u) {
            /* WiFi CAN: чуть разгрузить мост ESP. */
            Sleep(1);
        }
    }
    return 1;
}

static int RunVerifyBatch(const DeviceInfo *dev, uint32_t can_id, uint32_t batch_start, uint32_t batch_len,
                          const uint32_t *batch_words) {
    int max_retries = (batch_start == 0u) ? ACK_RETRY_FIRST_WORD : ACK_RETRY_NORMAL;
    int attempt;

    EnterCriticalSection(&g_ackCs);
    AckBatchBeginLocked(batch_start, batch_len, batch_words);
    LeaveCriticalSection(&g_ackCs);

    /* Вся пачка уходит подряд, ACK ждём по пачке целиком. Иначе параметр «Пачка»
     * не влияет на скорость (stop-and-wait по каждому слову). */
    for (attempt = 0; attempt < max_retries; attempt++) {
        uint32_t missing;

        if (InterlockedCompareExchange(&g_updateStop, 0, 0)) {
            goto fail;
        }

        EnterCriticalSection(&g_ackCs);
        missing = g_ack.batch_len - g_ack.acked_count;
        LeaveCriticalSection(&g_ackCs);
        if (missing == 0u) {
            break;
        }

        if (attempt > 0) {
            /* Не спамить лог на каждый retry — иначе конец обновления не видно. */
            if (attempt == 1 || (attempt % 2) == 0 || attempt + 1 == max_retries) {
                Logf(L"Повтор пачки %u-%u (нет ACK: %u из %u, попытка %d/%d)\r\n",
                     batch_start, batch_start + batch_len - 1u, missing, batch_len,
                     attempt + 1, max_retries);
            }
            /* Дать RS/ESP дойти ACK и не столкнуть следующий quad поверх дырки. */
            if (IsPanelDev(dev) && g_useTcp) {
                Sleep(ACK_RETRY_GAP_PANEL_MS);
            }
        }

        if (!SendUnackedBatchWords(dev, can_id, batch_start, batch_words)) {
            goto fail;
        }
        if (WaitAckBatch(batch_start)) {
            break;
        }
    }

    EnterCriticalSection(&g_ackCs);
    if (g_ack.acked_count < g_ack.batch_len) {
        uint32_t j;
        for (j = 0; j < g_ack.batch_len; j++) {
            if (!g_ack.acked[j]) {
                Logf(L"Нет подтверждения для слова %u\r\n", g_ack.expect_idx[j]);
                break;
            }
        }
        LeaveCriticalSection(&g_ackCs);
        goto fail;
    }
    g_ack.active = 0;
    LeaveCriticalSection(&g_ackCs);
    return 1;

fail:
    EnterCriticalSection(&g_ackCs);
    g_ack.active = 0;
    LeaveCriticalSection(&g_ackCs);
    return 0;
}

typedef struct {
    DeviceInfo dev;
    wchar_t file_path[MAX_PATH];
    int verify_packets;
    uint32_t batch_size;
} UpdaterArgs;

static uint32_t ReadBatchSizeFromUi(void) {
    wchar_t wbuf[32];
    wbuf[0] = L'\0';
    if (g_hBatch) GetWindowTextW(g_hBatch, wbuf, 31);
    long v = wcstol(wbuf, NULL, 10);
    if (v < (long)MIN_BATCH_SIZE) v = (long)MIN_BATCH_SIZE;
    if (v > (long)MAX_BATCH_SIZE) v = (long)MAX_BATCH_SIZE;
    return (uint32_t)v;
}

static void SetBatchUiEnabled(int enabled) {
    if (g_hBatch) EnableWindow(g_hBatch, enabled);
}

static int UpdaterAllocFailedCleanup(uint8_t *fw_buf, uint8_t *batch_buf, uint32_t *batch_words,
                                     UpdaterArgs *ua) {
    if (fw_buf) free(fw_buf);
    if (batch_buf) free(batch_buf);
    if (batch_words) free(batch_words);
    timeEndPeriod(1);
    EnterCriticalSection(&g_ackCs);
    g_ack.active = 0;
    LeaveCriticalSection(&g_ackCs);
    free(ua);
    InterlockedExchange(&g_updateRunning, 0);
    PostMessageW(g_hwnd, WM_APP_UPD_DONE, 0, 0);
    return 0;
}

static int WaitPanelEnterBoot(uint8_t rs_addr, uint8_t already_boot) {
    DWORD start;
    DWORD last_send;
    int send_count = 0;
    if (already_boot == RS_BUS_DEV_TYPE_PANEL_BOOTLOADER) {
        Logf(L"Панель уже в бутлоадере (RS=%u).\r\n", rs_addr);
        return 1;
    }
    g_panelWaitAddr = rs_addr;
    InterlockedExchange(&g_panelBootSeen, 0);
    InterlockedExchange(&g_panelEnterAcked, 0);
    Logf(L"Отправка ENTER_BOOTLOADER (0xF3) на RS=%u.\r\n", rs_addr);
    if (!SendPanelRsCmd(rs_addr, CMD_ENTER_BOOTLOADER, NULL, 0)) {
        Logf(L"Не удалось отправить 0xF3.\r\n");
        return 0;
    }
    send_count = 1;
    last_send = GetTickCount();
    start = last_send;
    while ((GetTickCount() - start) < ENTER_BOOT_WAIT_MS &&
           !InterlockedCompareExchange(&g_updateStop, 0, 0)) {
        if (InterlockedCompareExchange(&g_panelBootSeen, 0, 0)) {
            Logf(L"Панель в бутлоадере (ACTIVITY).\r\n");
            return 1;
        }
        /* Повтор каждые 1 с: единичный кадр через WiFi/ППКУ часто теряется. */
        if ((GetTickCount() - last_send) >= 1000u) {
            if (SendPanelRsCmd(rs_addr, CMD_ENTER_BOOTLOADER, NULL, 0)) {
                send_count++;
                Logf(L"Повтор 0xF3 (%d), ACK=%ld.\r\n",
                     send_count,
                     InterlockedCompareExchange(&g_panelEnterAcked, 0, 0));
            }
            last_send = GetTickCount();
        }
        Sleep(20);
    }
    if (InterlockedCompareExchange(&g_updateStop, 0, 0)) return 0;
    Logf(L"Таймаут ожидания бутлоадера панели (отправok=%d, ACK=%ld).\r\n",
         send_count,
         InterlockedCompareExchange(&g_panelEnterAcked, 0, 0));
    return 0;
}

static int WaitPanelTransmit(uint8_t rs_addr) {
    DWORD start;
    DWORD last_send;
    int send_count = 0;
    InterlockedExchange(&g_panelTransmitSeen, 0);
    InterlockedExchange(&g_panelTransmitOk, 0);
    g_panelWaitAddr = rs_addr;
    Logf(L"Отправка UPDATE_TRANSMIT (0x%02X) на RS=%u.\r\n", CMD_UPDATE_TRANSMIT, rs_addr);
    if (!SendPanelRsCmd(rs_addr, CMD_UPDATE_TRANSMIT, NULL, 0)) {
        Logf(L"Не удалось отправить UPDATE_TRANSMIT.\r\n");
        return 0;
    }
    send_count = 1;
    last_send = GetTickCount();
    start = last_send;
    while ((GetTickCount() - start) < PANEL_TRANSMIT_WAIT_MS &&
           !InterlockedCompareExchange(&g_updateStop, 0, 0)) {
        if (InterlockedCompareExchange(&g_panelTransmitSeen, 0, 0)) {
            if (InterlockedCompareExchange(&g_panelTransmitOk, 0, 0)) {
                Logf(L"UPDATE_TRANSMIT OK — образ принят, переход в приложение.\r\n");
                return 1;
            }
            Logf(L"UPDATE_TRANSMIT NACK — CRC/образ невалиден, панель остаётся в бутлоадере.\r\n");
            return 0;
        }
        if ((GetTickCount() - last_send) >= 2000u) {
            if (SendPanelRsCmd(rs_addr, CMD_UPDATE_TRANSMIT, NULL, 0)) {
                send_count++;
                Logf(L"Повтор UPDATE_TRANSMIT (%d).\r\n", send_count);
            }
            last_send = GetTickCount();
        }
        Sleep(20);
    }
    Logf(L"Таймаут UPDATE_TRANSMIT (отправok=%d) — ответ бута не получен.\r\n", send_count);
    return 0;
}

static DWORD WINAPI UpdaterThreadProc(LPVOID arg) {
    UpdaterArgs *ua = (UpdaterArgs *)arg;
    int is_panel = IsPanelDev(&ua->dev);
    int is_esp = IsEspDev(&ua->dev);
    FILE *fp = _wfopen(ua->file_path, L"rb");
    if (!fp) {
        PostMessageW(g_hwnd, WM_APP_UPD_DONE, 0, 0);
        free(ua);
        return 0;
    }
    fseek(fp, 0, SEEK_END);
    long fsz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsz <= 0) {
        fclose(fp);
        PostMessageW(g_hwnd, WM_APP_UPD_DONE, 0, 0);
        free(ua);
        return 0;
    }
    if (ua->dev.d_type == DEVICE_PPKY_TYPE && (unsigned long)fsz > PPKY_FW_MAX_BYTES) {
        Logf(L"Файл ППКУ больше 512 КБ (%ld байт).\r\n", fsz);
        fclose(fp);
        PostMessageW(g_hwnd, WM_APP_UPD_DONE, 0, 0);
        free(ua);
        return 0;
    }
    if (is_esp && (unsigned long)fsz > ESP_FW_MAX_BYTES) {
        Logf(L"Файл ESP32 больше 960 КБ (%ld байт, лимит слота OTA).\r\n", fsz);
        fclose(fp);
        PostMessageW(g_hwnd, WM_APP_UPD_DONE, 0, 0);
        free(ua);
        return 0;
    }
    if (is_panel && (unsigned long)fsz > PANEL_FW_MAX_BYTES) {
        Logf(L"Файл панели больше 256 КБ (%ld байт).\r\n", fsz);
        fclose(fp);
        PostMessageW(g_hwnd, WM_APP_UPD_DONE, 0, 0);
        free(ua);
        return 0;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)fsz);
    if (!buf) {
        fclose(fp);
        PostMessageW(g_hwnd, WM_APP_UPD_DONE, 0, 0);
        free(ua);
        return 0;
    }
    fread(buf, 1, (size_t)fsz, fp);
    fclose(fp);

    uint32_t total_words = ((uint32_t)fsz + 3u) / 4u;
    uint32_t can_id_req = BuildCanId(ua->dev.d_type, ua->dev.h_adr, ua->dev.l_adr, ua->dev.zone, 0);
    timeBeginPeriod(1);

    if (is_panel) {
        g_panelWaitAddr = ua->dev.l_adr;
        if (!WaitPanelEnterBoot(ua->dev.l_adr, ua->dev.rs_dev_type)) {
            Logf(L"ИТОГ: ОШИБКА — панель не вошла в бутлоадер.\r\n");
            free(buf);
            timeEndPeriod(1);
            EnterCriticalSection(&g_ackCs);
            g_ack.active = 0;
            LeaveCriticalSection(&g_ackCs);
            free(ua);
            InterlockedExchange(&g_updateRunning, 0);
            PostMessageW(g_hwnd, WM_APP_UPD_DONE, 0, 0);
            return 0;
        }
        if (ua->batch_size > PANEL_MAX_BATCH_SIZE) {
            Logf(L"Панель WiFi/RS: пачка ограничена до %u (было %u).\r\n",
                 PANEL_MAX_BATCH_SIZE, ua->batch_size);
            ua->batch_size = PANEL_MAX_BATCH_SIZE;
        }
        Logf(L"Панель: первое слово — erase flash (до ~35 с), дальше пачками по %u.\r\n",
             ua->batch_size);
    }

    SendMessageW(g_hProgress, PBM_SETRANGE32, 0, total_words);
    SendMessageW(g_hProgress, PBM_SETPOS, 0, 0);
    if (ua->verify_packets) {
        Logf(L"Старт обновления: words=%u, верификация=вкл, пачка=%u (ACK всей пачки)%s\r\n",
             total_words, ua->batch_size, is_panel ? L", RS/ESP_UART" : L"");
        if (ua->dev.d_type == DEVICE_PPKY_TYPE) {
            Logf(L"ППКУ: первое слово может занять до 35 с (стирание SPI).\r\n");
        }
        if (is_esp) {
            Logf(L"ESP32: первое слово может занять до 35 с (стирание OTA-слота).\r\n");
        }
    } else {
        Logf(L"Старт обновления: words=%u, верификация=выкл, пачка=%u%s\r\n",
             total_words, ua->batch_size, is_panel ? L", RS/ESP_UART" : L"");
    }

    size_t batch_cap = (size_t)ua->batch_size * BSU_PKT_SIZE_CAN;
    uint8_t *batch_buf = (uint8_t *)malloc(batch_cap);
    uint32_t *batch_words = (uint32_t *)malloc((size_t)ua->batch_size * sizeof(uint32_t));
    if (!batch_buf || !batch_words) {
        Logf(L"Ошибка выделения буфера пачки.\r\n");
        return UpdaterAllocFailedCleanup(buf, batch_buf, batch_words, ua);
    }

    uint32_t batch_start = 0;
    for (; batch_start < total_words; ) {
        if (InterlockedCompareExchange(&g_updateStop, 0, 0)) {
            Logf(L"Обновление остановлено пользователем.\r\n");
            break;
        }

        uint32_t batch_len = ua->batch_size;
        if (batch_start + batch_len > total_words) {
            batch_len = total_words - batch_start;
        }

        for (uint32_t j = 0; j < batch_len; j++) {
            batch_words[j] = ReadFirmwareWord(buf, fsz, batch_start + j);
        }

        int batch_ok = 0;
        int is_ppky = (ua->dev.d_type == DEVICE_PPKY_TYPE);
        if (ua->verify_packets) {
            /* Панель: слово 0 = erase flash до ~35 с. Не слать остальную пачку
             * до ACK слова 0 — иначе кадры копятся в RX бутлоадера под POLL/erase. */
            if ((is_panel || is_esp) && batch_start == 0u && batch_len > 1u) {
                Logf(L"%s: сначала слово 0 (erase), затем пачка 1..%u.\r\n",
                     is_esp ? L"ESP32" : L"Панель",
                     batch_len - 1u);
                batch_ok = RunVerifyBatch(&ua->dev, can_id_req, 0u, 1u, batch_words);
                if (batch_ok) {
                    batch_ok = RunVerifyBatch(&ua->dev, can_id_req, 1u, batch_len - 1u,
                                              batch_words + 1u);
                }
            } else {
                batch_ok = RunVerifyBatch(&ua->dev, can_id_req, batch_start, batch_len, batch_words);
            }
        } else if (is_panel || is_ppky || is_esp) {
            /* ППКУ/ESP на слово 0 стирают слот: пачка без паузы теряется. */
            batch_ok = 1;
            for (uint32_t j = 0; j < batch_len; j++) {
                if (!SendUpdateWord(&ua->dev, can_id_req, batch_start + j, batch_words[j])) {
                    batch_ok = 0;
                    Logf(L"Ошибка отправки слова %u (err=%u)\r\n",
                         batch_start + j, g_lastIoError);
                    break;
                }
                if ((is_ppky || is_esp) && (batch_start + j) == 0u) {
                    Logf(L"Слово 0: пауза 8 с на стирание слота %s.\r\n",
                         is_esp ? L"ESP32 OTA" : L"SPI ППКУ");
                    Sleep(8000);
                }
            }
            if (batch_ok && batch_start + batch_len < total_words) {
                Sleep(BATCH_SLEEP_MS);
            }
        } else {
            uint8_t payload[8];
            for (uint32_t j = 0; j < batch_len; j++) {
                BuildUpdateWordData(payload, batch_start + j, batch_words[j]);
                BuildBsuCanPacket(&batch_buf[j * BSU_PKT_SIZE_CAN], can_id_req, payload, BSU_PKT_TYPE_CAN1);
            }
            if (SerialWrite(batch_buf, (DWORD)(batch_len * BSU_PKT_SIZE_CAN))) {
                batch_ok = 1;
                if (batch_start + batch_len < total_words) {
                    Sleep(BATCH_SLEEP_MS);
                }
            } else {
                Logf(L"Ошибка отправки пачки (слова %u-%u, err=%u)\r\n",
                     batch_start, batch_start + batch_len - 1u, g_lastIoError);
            }
        }

        if (!batch_ok) break;

        batch_start += batch_len;
        PostMessageW(g_hProgress, PBM_SETPOS, batch_start, 0);
    }

    if (!InterlockedCompareExchange(&g_updateStop, 0, 0) && batch_start >= total_words) {
        if (is_panel) {
            if (WaitPanelTransmit(ua->dev.l_adr)) {
                Logf(L"ИТОГ: УСПЕХ — прошивка панели записана и запущена.\r\n");
            } else {
                Logf(L"ИТОГ: ОШИБКА — слова переданы, но старт приложения не подтверждён.\r\n");
                Logf(L"Панель, скорее всего, в бутлоадере; нужен повторный update _builder.bin.\r\n");
            }
        } else if (is_esp) {
            uint8_t endd[8] = { CMD_UPDATE_TRANSMIT, 0, 0, 0, 0, 0, 0, 0 };
            SendBsuEspCmd(endd, 8);
            Logf(L"Команда update_transmit отправлена на ESP32 (ESP_CMD).\r\n");
            Logf(L"ИТОГ: передача завершена (words=%u). ESP32 перезагрузится в новый образ.\r\n",
                 total_words);
        } else {
            uint8_t endd[8] = { CMD_UPDATE_TRANSMIT, 0, 0, 0, 0, 0, 0, 0 };
            SendBsuCanPacket(can_id_req, endd, BSU_PKT_TYPE_CAN1);
            Logf(L"Команда update_transmit отправлена.\r\n");
            Logf(L"ИТОГ: передача завершена (words=%u).\r\n", total_words);
        }
    } else if (InterlockedCompareExchange(&g_updateStop, 0, 0)) {
        Logf(L"ИТОГ: ОСТАНОВЛЕНО пользователем на слове %u из %u.\r\n",
             batch_start, total_words);
    } else {
        Logf(L"ИТОГ: ОШИБКА — прервано на слове %u из %u, update_transmit не отправлен.\r\n",
             batch_start, total_words);
        if (is_panel) {
            Logf(L"Образ панели неполный — устройство останется в бутлоадере до успешного обновления.\r\n");
        }
    }

    free(buf);
    free(batch_buf);
    free(batch_words);
    timeEndPeriod(1);
    EnterCriticalSection(&g_ackCs);
    g_ack.active = 0;
    LeaveCriticalSection(&g_ackCs);
    free(ua);
    InterlockedExchange(&g_updateRunning, 0);
    PostMessageW(g_hwnd, WM_APP_UPD_DONE, 0, 0);
    return 0;
}

static void StartUpdate(void) {
    if (!g_selectedTargetValid) {
        MessageBoxW(g_hwnd, L"Сначала выберите цель кнопкой \"Цель обновления...\".", L"Updater", MB_ICONWARNING);
        return;
    }
    wchar_t path[MAX_PATH] = {0};
    GetWindowTextW(g_hFile, path, MAX_PATH - 1);
    if (path[0] == 0) {
        MessageBoxW(g_hwnd, L"Выберите файл прошивки.", L"Updater", MB_ICONWARNING);
        return;
    }
    {
        HANDLE hf = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf == INVALID_HANDLE_VALUE) {
            MessageBoxW(g_hwnd, L"Не удалось открыть файл прошивки.", L"Updater", MB_ICONERROR);
            return;
        }
        DWORD fsz = GetFileSize(hf, NULL);
        CloseHandle(hf);
        if (fsz == INVALID_FILE_SIZE) {
            MessageBoxW(g_hwnd, L"Не удалось определить размер файла.", L"Updater", MB_ICONERROR);
            return;
        }
        if (g_selectedTargetDev.d_type == DEVICE_PPKY_TYPE && fsz > PPKY_FW_MAX_BYTES) {
            MessageBoxW(g_hwnd, L"Размер файла ППКУ больше 512 КБ (лимит слота UPDATE).", L"Updater", MB_ICONWARNING);
            return;
        }
        if (g_selectedTargetDev.d_type == DEVICE_ESP32_TYPE && fsz > ESP_FW_MAX_BYTES) {
            MessageBoxW(g_hwnd, L"Размер файла ESP32 больше 960 КБ (лимит слота OTA).", L"Updater", MB_ICONWARNING);
            return;
        }
        if (g_selectedTargetDev.d_type == DEVICE_PANEL_TYPE && fsz > PANEL_FW_MAX_BYTES) {
            MessageBoxW(g_hwnd, L"Размер файла панели больше 256 КБ.", L"Updater", MB_ICONWARNING);
            return;
        }
    }
    if (g_hUpdaterThread) {
        MessageBoxW(g_hwnd, L"Обновление уже запущено.", L"Updater", MB_ICONINFORMATION);
        return;
    }
    UpdaterArgs *ua = (UpdaterArgs *)calloc(1, sizeof(UpdaterArgs));
    if (!ua) return;
    ua->dev = g_selectedTargetDev;
    g_activeUpdateDev = ua->dev;
    g_activeUpdateDevValid = 1;
    wcsncpy(ua->file_path, path, MAX_PATH - 1);
    ua->verify_packets = (g_hVerify && SendMessageW(g_hVerify, BM_GETCHECK, 0, 0) == BST_CHECKED);
    ua->batch_size = ReadBatchSizeFromUi();
    if (ua->dev.d_type == DEVICE_PANEL_TYPE && ua->batch_size > PANEL_MAX_BATCH_SIZE) {
        ua->batch_size = PANEL_MAX_BATCH_SIZE;
    }
    InterlockedExchange(&g_updateStop, 0);
    InterlockedExchange(&g_updateRunning, 1);
    EnableWindow(g_hStart, FALSE);
    EnableWindow(g_hStop, TRUE);
    if (g_hVerify) EnableWindow(g_hVerify, FALSE);
    SetBatchUiEnabled(FALSE);
    g_hUpdaterThread = CreateThread(NULL, 0, UpdaterThreadProc, ua, 0, NULL);
}

static void StopUpdate(void) {
    InterlockedExchange(&g_updateStop, 1);
    if (g_activeUpdateDevValid && InterlockedCompareExchange(&g_connected, 0, 0)) {
        if (IsPanelDev(&g_activeUpdateDev)) {
            (void)SendPanelRsCmd(g_activeUpdateDev.l_adr, CMD_UPDATE_TRANSMIT, NULL, 0);
        } else if (IsEspDev(&g_activeUpdateDev)) {
            uint8_t endd[8] = { CMD_UPDATE_TRANSMIT, 0, 0, 0, 0, 0, 0, 0 };
            SendBsuEspCmd(endd, 8);
        } else {
            uint32_t can_id_req = BuildCanId(g_activeUpdateDev.d_type, g_activeUpdateDev.h_adr,
                                             g_activeUpdateDev.l_adr, g_activeUpdateDev.zone, 0);
            uint8_t endd[8] = { CMD_UPDATE_TRANSMIT, 0, 0, 0, 0, 0, 0, 0 };
            SendBsuCanPacket(can_id_req, endd, BSU_PKT_TYPE_CAN1);
        }
        Logf(L"Принудительное завершение: update_transmit отправлен.\r\n");
    }
}

typedef struct {
    const wchar_t *tag;
    const wchar_t *proj_dir;
} MkuCollectEntry;

static const MkuCollectEntry g_mku_collect[] = {
    { L"K1", L"MCU_k1_v097" },
    { L"K2", L"MCU_k2_v097" },
    { L"K3", L"MCU_k3_v097" },
    { L"KR", L"MCU_kr_v095" },
};

static int EnsureDirectoryExists(const wchar_t *path) {
    DWORD attr = GetFileAttributesW(path);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return 1;
    return CreateDirectoryW(path, NULL) != 0;
}

static void ClearDirectoryFiles(const wchar_t *dir) {
    wchar_t pattern[MAX_PATH];
    wsprintfW(pattern, L"%s\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == L'\0' || (fd.cFileName[1] == L'.' && fd.cFileName[2] == L'\0'))) {
            continue;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        wchar_t fp[MAX_PATH];
        wsprintfW(fp, L"%s\\%s", dir, fd.cFileName);
        DeleteFileW(fp);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static int ReadTextFileA(const wchar_t *path, char *buf, size_t buf_sz) {
    FILE *fp = _wfopen(path, L"rb");
    if (!fp) return 0;
    size_t n = fread(buf, 1, buf_sz - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    return (int)(n > 0);
}

static int ParseUniqDebugFromMainH(const wchar_t *proj_dir, int *uniq_debug) {
    wchar_t main_h[MAX_PATH];
    char text[16384];
    wsprintfW(main_h, L"%s%s\\Core\\Inc\\main.h", FW_WORKSPACE_ROOT, proj_dir);
    if (!ReadTextFileA(main_h, text, sizeof(text))) return 0;
    const char *p = strstr(text, "#define UNIQ_DEBUG");
    if (!p) return 0;
    p += strlen("#define UNIQ_DEBUG");
    while (*p == ' ' || *p == '\t') p++;
    *uniq_debug = atoi(p);
    return 1;
}

static int ParseAppVersionU32(const wchar_t *proj_dir, uint32_t *version) {
    wchar_t upd_path[MAX_PATH];
    char text[8192];
    wsprintfW(upd_path, L"%s%s\\Core\\Src\\upd.cpp", FW_WORKSPACE_ROOT, proj_dir);
    if (!ReadTextFileA(upd_path, text, sizeof(text))) return 0;

    const char *p = strstr(text, "#define APP_VERSION_U32");
    if (!p) return 0;
    p += strlen("#define APP_VERSION_U32");
    while (*p == ' ' || *p == '\t') p++;

    long base = 0;
    if (sscanf(p, "%ld", &base) != 1) return 0;

    if (strstr(p, "UNIQ_DEBUG")) {
        int uniq_debug = 0;
        if (!ParseUniqDebugFromMainH(proj_dir, &uniq_debug)) uniq_debug = 0;
        *version = (uint32_t)(base + uniq_debug * 100);
    } else {
        *version = (uint32_t)base;
    }
    return 1;
}

static int FindBuilderBinInProject(const wchar_t *proj_dir, wchar_t *out_path, size_t out_chars) {
    static const wchar_t *configs[] = { L"Debug", L"Release" };
    wchar_t best_path[MAX_PATH];
    best_path[0] = L'\0';
    FILETIME best_time = {0};
    int found = 0;

    for (size_t ci = 0; ci < sizeof(configs) / sizeof(configs[0]); ci++) {
        wchar_t pattern[MAX_PATH];
        wsprintfW(pattern, L"%s%s\\%s\\*_builder.bin", FW_WORKSPACE_ROOT, proj_dir, configs[ci]);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            wchar_t full[MAX_PATH];
            wsprintfW(full, L"%s%s\\%s\\%s", FW_WORKSPACE_ROOT, proj_dir, configs[ci], fd.cFileName);
            if (!found || CompareFileTime(&fd.ftLastWriteTime, &best_time) > 0) {
                wcsncpy(best_path, full, MAX_PATH - 1);
                best_path[MAX_PATH - 1] = L'\0';
                best_time = fd.ftLastWriteTime;
                found = 1;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    if (!found) return 0;
    wcsncpy(out_path, best_path, out_chars - 1);
    out_path[out_chars - 1] = L'\0';
    return 1;
}

static void CollectFirmwares(void) {
    if (!EnsureDirectoryExists(FW_COLLECT_DST)) {
        MessageBoxW(g_hwnd, L"Не удалось создать папку назначения.", L"Собрать", MB_ICONERROR);
        return;
    }

    ClearDirectoryFiles(FW_COLLECT_DST);
    Logf(L"Сборка прошивок МКУ в %s\r\n", FW_COLLECT_DST);

    int ok_count = 0;
    int fail_count = 0;

    for (size_t i = 0; i < sizeof(g_mku_collect) / sizeof(g_mku_collect[0]); i++) {
        const MkuCollectEntry *e = &g_mku_collect[i];
        wchar_t src[MAX_PATH];
        uint32_t ver = 0;

        if (!FindBuilderBinInProject(e->proj_dir, src, MAX_PATH)) {
            Logf(L"[ОШИБКА] %s: не найден *_builder.bin\r\n", e->tag);
            fail_count++;
            continue;
        }
        if (!ParseAppVersionU32(e->proj_dir, &ver)) {
            Logf(L"[ОШИБКА] %s: не удалось прочитать APP_VERSION_U32\r\n", e->tag);
            fail_count++;
            continue;
        }

        wchar_t dst[MAX_PATH];
        wsprintfW(dst, L"%s\\MKU_%s_v%u_builder.bin", FW_COLLECT_DST, e->tag, ver);
        if (!CopyFileW(src, dst, FALSE)) {
            Logf(L"[ОШИБКА] %s: копирование не удалось\r\n", e->tag);
            fail_count++;
            continue;
        }

        Logf(L"[OK] %s v%u ← %s\r\n", e->tag, ver, src);
        ok_count++;
    }

    Logf(L"Сборка завершена: успешно %d, ошибок %d\r\n", ok_count, fail_count);
    if (fail_count > 0) {
        MessageBoxW(g_hwnd, L"Сборка завершена с ошибками. См. лог.", L"Собрать", MB_ICONWARNING);
    } else if (ok_count == 0) {
        MessageBoxW(g_hwnd, L"Не скопировано ни одного файла.", L"Собрать", MB_ICONWARNING);
    } else {
        MessageBoxW(g_hwnd, L"Все прошивки МКУ собраны.", L"Собрать", MB_ICONINFORMATION);
    }
}

static void BrowseFile(void) {
    OPENFILENAMEW ofn = {0};
    wchar_t file[MAX_PATH] = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Firmware builder (*_builder.bin)\0*_builder.bin\0Firmware (*.bin)\0*.bin\0All files\0*.*\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) SetWindowTextW(g_hFile, file);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_hwnd = hwnd;
            InitCommonControls();
            RECT rc; GetClientRect(hwnd, &rc);
            int w = rc.right - rc.left;

            g_hMode = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                                    10, 10, 80, 200, hwnd, (HMENU)IDC_COMBO_MODE, NULL, NULL);
            SendMessageW(g_hMode, CB_ADDSTRING, 0, (LPARAM)L"USB");
            SendMessageW(g_hMode, CB_ADDSTRING, 0, (LPARAM)L"WiFi");
            SendMessageW(g_hMode, CB_SETCURSEL, 0, 0);
            g_hPort = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 95, 10, 90, 300, hwnd, (HMENU)IDC_COMBO_PORT, NULL, NULL);
            CreateWindowW(L"BUTTON", L"Порты", WS_CHILD | WS_VISIBLE, 190, 10, 60, 24, hwnd, (HMENU)IDC_BTN_REFRESH, NULL, NULL);
            g_hWifiHost = CreateWindowW(L"EDIT", WIFI_DEFAULT_HOST, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                        255, 10, 120, 24, hwnd, (HMENU)IDC_EDIT_WIFI_HOST, NULL, NULL);
            g_hWifiPort = CreateWindowW(L"EDIT", WIFI_DEFAULT_PORT, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER | ES_CENTER,
                                        380, 10, 50, 24, hwnd, (HMENU)IDC_EDIT_WIFI_PORT, NULL, NULL);
            g_hConnect = CreateWindowW(L"BUTTON", L"Подключить", WS_CHILD | WS_VISIBLE, 435, 10, 100, 24, hwnd, (HMENU)IDC_BTN_CONNECT, NULL, NULL);
            CreateWindowW(L"BUTTON", L"Цель обновления...", WS_CHILD | WS_VISIBLE, 545, 10, 145, 24, hwnd, (HMENU)IDC_BTN_SELECT_TARGET, NULL, NULL);
            CreateWindowW(L"BUTTON", L"Прочитать версии", WS_CHILD | WS_VISIBLE, 695, 10, 130, 24, hwnd, (HMENU)IDC_BTN_FORCE_VERSION, NULL, NULL);
            CreateWindowW(L"BUTTON", L"Собрать", WS_CHILD | WS_VISIBLE, 830, 10, 80, 24, hwnd, (HMENU)IDC_BTN_COLLECT, NULL, NULL);

            g_hDevices = CreateWindowW(WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | WS_BORDER,
                                       10, 44, w - 20, 180, hwnd, (HMENU)IDC_LIST_DEVICES, NULL, NULL);
            ListView_SetExtendedListViewStyle(g_hDevices, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
            LVCOLUMNW c = {0}; c.mask = LVCF_TEXT | LVCF_WIDTH;
            c.cx = 180; c.pszText = L"Устройство"; ListView_InsertColumn(g_hDevices, 0, &c);
            c.cx = 70; c.pszText = L"h_adr";  ListView_InsertColumn(g_hDevices, 1, &c);
            c.cx = 70; c.pszText = L"l_adr";  ListView_InsertColumn(g_hDevices, 2, &c);
            c.cx = 70; c.pszText = L"zone";   ListView_InsertColumn(g_hDevices, 3, &c);
            c.cx = 320; c.pszText = L"Версия"; ListView_InsertColumn(g_hDevices, 4, &c);

            g_hFile = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                    10, 232, w - 140, 24, hwnd, (HMENU)IDC_EDIT_FILE, NULL, NULL);
            CreateWindowW(L"BUTTON", L"Файл...", WS_CHILD | WS_VISIBLE, w - 120, 232, 110, 24, hwnd, (HMENU)IDC_BTN_BROWSE, NULL, NULL);

            g_hStart = CreateWindowW(L"BUTTON", L"Старт обновления", WS_CHILD | WS_VISIBLE, 10, 264, 150, 28, hwnd, (HMENU)IDC_BTN_START, NULL, NULL);
            g_hStop = CreateWindowW(L"BUTTON", L"Принудительно завершить", WS_CHILD | WS_VISIBLE, 170, 264, 180, 28, hwnd, (HMENU)IDC_BTN_STOP, NULL, NULL);
            EnableWindow(g_hStop, FALSE);
            g_hVerify = CreateWindowW(L"BUTTON", L"Верификация (ACK)",
                                      WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                      360, 264, 150, 28, hwnd, (HMENU)IDC_CHK_VERIFY, NULL, NULL);
            SendMessageW(g_hVerify, BM_SETCHECK, BST_CHECKED, 0);
            CreateWindowW(L"STATIC", L"Пачка:",
                          WS_CHILD | WS_VISIBLE, 520, 268, 50, 20, hwnd, NULL, NULL, NULL);
            g_hBatch = CreateWindowW(L"EDIT", L"8",
                                     WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER | ES_CENTER,
                                     570, 264, 50, 24, hwnd, (HMENU)IDC_EDIT_BATCH, NULL, NULL);

            g_hProgress = CreateWindowW(PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE, 10, 298, w - 20, 20, hwnd, (HMENU)IDC_PROGRESS, NULL, NULL);
            g_hLog = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
                                   10, 324, w - 20, rc.bottom - 334, hwnd, (HMENU)IDC_EDIT_LOG, NULL, NULL);

            RefreshPorts();
            UpdateTransportUi();
            SetTimer(hwnd, IDT_VERSION_RETRY, 1000, NULL);
            break;
        }
        case WM_SIZE: {
            RECT rc; GetClientRect(hwnd, &rc);
            int w = rc.right - rc.left;
            MoveWindow(g_hDevices, 10, 44, w - 20, 180, TRUE);
            MoveWindow(g_hFile, 10, 232, w - 140, 24, TRUE);
            MoveWindow(GetDlgItem(hwnd, IDC_BTN_BROWSE), w - 120, 232, 110, 24, TRUE);
            MoveWindow(g_hProgress, 10, 298, w - 20, 20, TRUE);
            MoveWindow(g_hLog, 10, 324, w - 20, rc.bottom - 334, TRUE);
            break;
        }
        case WM_TIMER:
            if (wp == IDT_VERSION_RETRY)
                RetryIncompleteVersions();
            break;
        case WM_COMMAND: {
            if (HIWORD(wp) == CBN_SELCHANGE && LOWORD(wp) == IDC_COMBO_MODE) {
                UpdateTransportUi();
                break;
            }
            switch (LOWORD(wp)) {
                case IDC_BTN_REFRESH: RefreshPorts(); break;
                case IDC_BTN_BROWSE: BrowseFile(); break;
                case IDC_BTN_CONNECT:
                    if (!InterlockedCompareExchange(&g_connected, 0, 0)) {
                        int ok = IsWifiMode() ? ConnectTcp() : ConnectSerial();
                        if (ok) {
                            SetConnectedUi(1);
                            EnsureEsp32Device();
                            Logf(IsWifiMode() ? L"Подключено по WiFi. Список устройств очищен.\r\n"
                                              : L"Подключено по USB. Список устройств очищен.\r\n");
                            Logf(L"ESP32 добавлен в список устройств (команды 159/156/158 через ESP_CMD).\r\n");
                        } else {
                            MessageBoxW(hwnd,
                                        IsWifiMode() ? L"Ошибка подключения по WiFi (TCP)."
                                                     : L"Ошибка подключения к COM.",
                                        L"Updater", MB_ICONERROR);
                        }
                    } else {
                        DisconnectSerial();
                        SetConnectedUi(0);
                        Logf(L"Отключено.\r\n");
                    }
                    break;
                case IDC_BTN_SELECT_TARGET: SelectUpdateTarget(); break;
                case IDC_BTN_FORCE_VERSION: ForceReadVersions(); break;
                case IDC_BTN_COLLECT: CollectFirmwares(); break;
                case IDC_BTN_START: StartUpdate(); break;
                case IDC_BTN_STOP: StopUpdate(); break;
            }
            break;
        }
        case WM_APP_PACKET: {
            PostedPacket *pp = (PostedPacket *)lp;
            if (!pp) break;
            if (pp->kind == 2) {
                if (pp->data[0] == CMD_GET_VERSION) {
                    int idx = -1;
                    DeviceInfo d = {0};
                    d.d_type = DEVICE_ESP32_TYPE;
                    d.last_seen_ms = GetTickCount();
                    idx = FindOrAddDevice(d);
                    if (idx >= 0) {
                        g_devices[idx].d_type = DEVICE_ESP32_TYPE;
                        g_devices[idx].last_seen_ms = d.last_seen_ms;
                        HandleVersionPacket(&g_devices[idx], pp->data);
                        if (idx >= ListView_GetItemCount(g_hDevices)) AddDeviceToList(idx);
                        else RefreshDeviceListRow(idx);
                    }
                }
                free(pp);
                break;
            }
            if (pp->kind == 1) {
                if ((pp->rs_flags & RS_BUS_FLAG_DIR) != 0 &&
                    pp->rs_cmd == RS_PANEL_RSP_ACTIVITY &&
                    pp->rs_payload_len >= 10u) {
                    DeviceInfo d = {0};
                    uint16_t fw_ver = (uint16_t)(pp->rs_payload[1] | ((uint16_t)pp->rs_payload[2] << 8));
                    d.d_type = DEVICE_PANEL_TYPE;
                    d.h_adr = 0;
                    d.l_adr = pp->rs_addr;
                    d.zone = 0;
                    d.rs_dev_type = pp->rs_payload[0];
                    d.last_seen_ms = GetTickCount();
                    d.version = fw_ver;
                    d.version_valid = 1;
                    if (d.rs_dev_type == RS_BUS_DEV_TYPE_PANEL_BOOTLOADER)
                        strncpy(d.version_str, "boot", sizeof(d.version_str) - 1);
                    else
                        _snprintf(d.version_str, sizeof(d.version_str) - 1, "fw=%u", (unsigned)fw_ver);
                    {
                        int idx = FindOrAddDevice(d);
                        if (idx >= 0) {
                            g_devices[idx].d_type = d.d_type;
                            g_devices[idx].h_adr = d.h_adr;
                            g_devices[idx].l_adr = d.l_adr;
                            g_devices[idx].zone = d.zone;
                            g_devices[idx].rs_dev_type = d.rs_dev_type;
                            g_devices[idx].last_seen_ms = d.last_seen_ms;
                            g_devices[idx].version = d.version;
                            g_devices[idx].version_valid = 1;
                            strncpy(g_devices[idx].version_str, d.version_str,
                                    sizeof(g_devices[idx].version_str) - 1);
                            if (idx >= ListView_GetItemCount(g_hDevices)) AddDeviceToList(idx);
                            else RefreshDeviceListRow(idx);
                        }
                    }
                }
                if ((pp->rs_flags & RS_BUS_FLAG_DIR) != 0 && pp->rs_cmd == CMD_GET_VERSION) {
                    for (int i = 0; i < g_devCount; i++) {
                        if (g_devices[i].d_type == DEVICE_PANEL_TYPE &&
                            g_devices[i].l_adr == pp->rs_addr) {
                            size_t n = pp->rs_payload_len;
                            if (n >= sizeof(g_devices[i].version_str))
                                n = sizeof(g_devices[i].version_str) - 1;
                            memcpy(g_devices[i].version_str, pp->rs_payload, n);
                            g_devices[i].version_str[n] = '\0';
                            ParseVersionString(&g_devices[i]);
                            RefreshDeviceListRow(i);
                            break;
                        }
                    }
                }
                free(pp);
                break;
            }
            CanIdFields f = ParseCanId(pp->can_id);
            if (f.dir == 1) {
                if (IsUpdatableType(f.d_type)) {
                    DeviceInfo d = {0};
                    d.d_type = f.d_type;
                    d.h_adr = f.h_adr;
                    d.l_adr = f.l_adr;
                    d.zone = f.zone;
                    d.last_seen_ms = GetTickCount();
                    int idx = FindOrAddDevice(d);
                    if (idx >= 0) {
                        uint8_t is_ver_pkt = (pp->data[0] == CMD_GET_VERSION) ? 1u : 0u;
                        uint8_t need_version_request = 0;
                        if (g_devices[idx].d_type == 0) g_devices[idx] = d;
                        else {
                            g_devices[idx].d_type = d.d_type;
                            g_devices[idx].h_adr = d.h_adr;
                            g_devices[idx].l_adr = d.l_adr;
                            g_devices[idx].zone = d.zone;
                            g_devices[idx].last_seen_ms = d.last_seen_ms;
                        }
                        /* Строка в списке — сразу, версия не обязательна. 159 — один раз и не из этого же обработчика. */
                        if (!is_ver_pkt &&
                            !g_devices[idx].version_valid &&
                            !g_devices[idx].version_req_sent) {
                            g_devices[idx].version_req_sent = 1;
                            need_version_request = 1;
                        }
                        if (idx >= ListView_GetItemCount(g_hDevices)) AddDeviceToList(idx);
                        else RefreshDeviceListRow(idx);
                        if (need_version_request)
                            PostMessageW(g_hwnd, WM_APP_REQ_VERSION, (WPARAM)idx, 0);
                    }
                }
            }
            if (f.dir == 1 && pp->data[0] == CMD_GET_VERSION) {
                for (int i = 0; i < g_devCount; i++) {
                    if (g_devices[i].d_type == f.d_type &&
                        g_devices[i].h_adr == f.h_adr &&
                        g_devices[i].l_adr == f.l_adr &&
                        g_devices[i].zone == f.zone) {
                        HandleVersionPacket(&g_devices[i], pp->data);
                        RefreshDeviceListRow(i);
                        break;
                    }
                }
            }
            free(pp);
            break;
        }
        case WM_APP_REQ_VERSION: {
            int idx = (int)wp;
            if (idx >= 0 && idx < g_devCount &&
                InterlockedCompareExchange(&g_connected, 0, 0)) {
                RequestDeviceVersion(&g_devices[idx]);
            }
            break;
        }
        case WM_APP_UPD_DONE:
            if (g_hUpdaterThread) {
                CloseHandle(g_hUpdaterThread);
                g_hUpdaterThread = NULL;
            }
            EnableWindow(g_hStart, TRUE);
            EnableWindow(g_hStop, FALSE);
            if (g_hVerify) EnableWindow(g_hVerify, TRUE);
            SetBatchUiEnabled(TRUE);
            /* После update_transmit МКУ перезагружается, и версия могла измениться:
             * сбрасываем кэш и инициируем повторный опрос целевого МКУ. */
            if (g_activeUpdateDevValid) {
                InvalidateDeviceVersionCache(&g_activeUpdateDev);
                if (InterlockedCompareExchange(&g_connected, 0, 0)) {
                    RequestDeviceVersion(&g_activeUpdateDev);
                }
            }
            g_activeUpdateDevValid = 0;
            Logf(L"Обновление завершено.\r\n");
            break;
        case WM_DESTROY:
            KillTimer(hwnd, IDT_VERSION_RETRY);
            StopUpdate();
            if (g_hUpdaterThread) {
                WaitForSingleObject(g_hUpdaterThread, 1000);
                CloseHandle(g_hUpdaterThread);
                g_hUpdaterThread = NULL;
            }
            DisconnectSerial();
            WSACleanup();
            DeleteCriticalSection(&g_ioCs);
            DeleteCriticalSection(&g_ackCs);
            PostQuitMessage(0);
            break;
        default: return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return 0;
}

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE hp, LPWSTR cmd, int nCmdShow) {
    (void)hp; (void)cmd;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        MessageBoxW(NULL, L"WSAStartup failed.", L"Updater", MB_ICONERROR);
        return 1;
    }
    InitializeCriticalSection(&g_ioCs);
    InitializeCriticalSection(&g_ackCs);
    InitializeConditionVariable(&g_ackCv);

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"BsuUpdaterWnd";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowW(wc.lpszClassName, APP_TITLE, WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1100, 640, NULL, NULL, hInst, NULL);
    if (!hwnd) {
        WSACleanup();
        return 1;
    }
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

