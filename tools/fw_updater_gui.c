#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#pragma comment(lib, "comctl32.lib")

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

#define WM_APP_LOG          (WM_APP + 1)
#define WM_APP_PACKET       (WM_APP + 2)
#define WM_APP_UPD_DONE     (WM_APP + 3)

#define BSU_PKT_TYPE_CAN1   0u
#define BSU_PREAMBLE0       0x55u
#define BSU_PREAMBLE1       0xAAu
#define BSU_PKT_SIZE_CAN    22u

#define CMD_SET_UPDATE_WORD 156u
#define CMD_UPDATE_TRANSMIT 158u
#define CMD_GET_VERSION     159u
#define ACK_WAIT_MS_FIRST_WORD 2500u
#define ACK_WAIT_MS_NORMAL 30u
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
} DeviceInfo;

typedef struct {
    int active;
    uint32_t expect_word_idx;
    uint32_t expect_word_value;
    int matched;
} AckState;

static HWND g_hwnd = NULL;
static HWND g_hPort = NULL, g_hConnect = NULL, g_hDevices = NULL, g_hFile = NULL;
static HWND g_hStart = NULL, g_hStop = NULL, g_hLog = NULL, g_hProgress = NULL;

static HANDLE g_hSerial = INVALID_HANDLE_VALUE;
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
    uint32_t can_id;
    uint8_t data[8];
    uint8_t bus_label;
} PostedPacket;

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

static void HandleAckFastPath(uint32_t can_id, const uint8_t data[8]) {
    CanIdFields f = ParseCanId(can_id);
    if (f.dir != 1) return;
    if (data[0] != CMD_SET_UPDATE_WORD) return;

    uint32_t idx = ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
    uint32_t word = ((uint32_t)data[4] << 24) | ((uint32_t)data[5] << 16) |
                    ((uint32_t)data[6] << 8) | data[7];

    EnterCriticalSection(&g_ackCs);
    if (g_ack.active && idx == g_ack.expect_word_idx && word == g_ack.expect_word_value) {
        g_ack.matched = 1;
        WakeAllConditionVariable(&g_ackCv);
    }
    LeaveCriticalSection(&g_ackCs);
}

static int SerialWrite(const uint8_t *buf, DWORD sz) {
    if (g_hSerial == INVALID_HANDLE_VALUE) return 0;
    EnterCriticalSection(&g_ioCs);
    DWORD wr = 0;
    BOOL ok = WriteFile(g_hSerial, buf, sz, &wr, NULL);
    LeaveCriticalSection(&g_ioCs);
    return ok && wr == sz;
}

static int SendBsuCanPacket(uint32_t can_id, const uint8_t data[8], uint8_t bus_type) {
    uint8_t pkt[BSU_PKT_SIZE_CAN];
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
    return SerialWrite(pkt, sizeof(pkt));
}

static void RefreshPorts(void) {
    SendMessageW(g_hPort, CB_RESETCONTENT, 0, 0);
    for (int i = 1; i <= 30; i++) {
        wchar_t p[16];
        wsprintfW(p, L"COM%d", i);
        SendMessageW(g_hPort, CB_ADDSTRING, 0, (LPARAM)p);
    }
    SendMessageW(g_hPort, CB_SETCURSEL, 0, 0);
}

static void SetConnectedUi(int connected) {
    EnableWindow(g_hPort, !connected);
    EnableWindow(GetDlgItem(g_hwnd, IDC_BTN_REFRESH), !connected);
    SetWindowTextW(g_hConnect, connected ? L"Отключить" : L"Подключить");
}

static DWORD WINAPI ReaderThreadProc(LPVOID arg) {
    (void)arg;
    enum {S_P0, S_P1, S_S0, S_S1, S_T0, S_T1, S_Q0, S_Q1, S_BODY, S_C0, S_C1} st = S_P0;
    uint16_t size = 0, type = 0, calc = 0, recv = 0;
    uint8_t body[64];
    int body_need = 0, body_idx = 0;
    uint8_t c0 = 0;
    uint8_t rxbuf[512];
    uint8_t b = 0;
    DWORD rd = 0;

    while (!InterlockedCompareExchange(&g_readerStop, 0, 0)) {
        if (!ReadFile(g_hSerial, rxbuf, sizeof(rxbuf), &rd, NULL) || rd == 0) {
            Sleep(1);
            continue;
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
                    if (body_need < 12 || body_need > 64 || (type != 0 && type != 1)) st = S_P0;
                    else st = S_BODY;
                    break;
                case S_BODY:
                    body[body_idx++] = b;
                    calc += b;
                    if (body_idx >= body_need) st = S_C0;
                    break;
                case S_C0: c0 = b; st = S_C1; break;
                case S_C1:
                    recv = (uint16_t)c0 | ((uint16_t)b << 8);
                    if (((uint16_t)calc) == recv && body_need >= 12) {
                        uint32_t can_id = (uint32_t)body[0] | ((uint32_t)body[1] << 8) |
                                          ((uint32_t)body[2] << 16) | ((uint32_t)body[3] << 24);
                        HandleAckFastPath(can_id, &body[4]);
                        CanIdFields f = ParseCanId(can_id);
                        int is_ack_packet = (f.dir == 1 && body[4] == CMD_SET_UPDATE_WORD);
                        /* Во время обновления не засоряем UI второстепенными пакетами,
                         * чтобы поток чтения оставался максимально быстрым. */
                        if (!InterlockedCompareExchange(&g_updateRunning, 0, 0) || !is_ack_packet) {
                            PostedPacket *pp = (PostedPacket *)malloc(sizeof(PostedPacket));
                            if (pp) {
                                pp->can_id = can_id;
                                memcpy(pp->data, &body[4], 8);
                                pp->bus_label = (uint8_t)type;
                                PostMessageW(g_hwnd, WM_APP_PACKET, 0, (LPARAM)pp);
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

static int ConnectSerial(void) {
    wchar_t port[64];
    GetWindowTextW(g_hPort, port, 63);
    wchar_t path[80];
    wsprintfW(path, L"\\\\.\\%s", port);

    g_hSerial = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (g_hSerial == INVALID_HANDLE_VALUE) return 0;

    DCB dcb = {0};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(g_hSerial, &dcb)) return 0;
    dcb.BaudRate = 1000000;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    if (!SetCommState(g_hSerial, &dcb)) return 0;

    COMMTIMEOUTS to = {0};
    /* Truly nonblocking read: ReadFile возвращает сразу, если данных нет. */
    to.ReadIntervalTimeout = MAXDWORD;
    to.ReadTotalTimeoutConstant = 0;
    to.ReadTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant = 200;
    to.WriteTotalTimeoutMultiplier = 0;
    SetCommTimeouts(g_hSerial, &to);
    PurgeComm(g_hSerial, PURGE_RXCLEAR | PURGE_TXCLEAR);

    InterlockedExchange(&g_readerStop, 0);
    g_hReaderThread = CreateThread(NULL, 0, ReaderThreadProc, NULL, 0, NULL);
    if (!g_hReaderThread) return 0;
    SetThreadPriority(g_hReaderThread, THREAD_PRIORITY_ABOVE_NORMAL);
    InterlockedExchange(&g_connected, 1);
    return 1;
}

static void DisconnectSerial(void) {
    InterlockedExchange(&g_readerStop, 1);
    if (g_hReaderThread) {
        WaitForSingleObject(g_hReaderThread, 1000);
        CloseHandle(g_hReaderThread);
        g_hReaderThread = NULL;
    }
    if (g_hSerial != INVALID_HANDLE_VALUE) {
        CloseHandle(g_hSerial);
        g_hSerial = INVALID_HANDLE_VALUE;
    }
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
        case 13: return L"МКУ_IGN";
        case 14: return L"МКУ_TC";
        case 20: return L"МКУ_K1";
        case 21: return L"МКУ_K2";
        case 22: return L"МКУ_K3";
        case 23: return L"МКУ_KR";
        default: return L"МКУ";
    }
}

static void RefreshDeviceListRow(int idx) {
    if (idx < 0 || idx >= g_devCount) return;
    wchar_t text[64];
    LVITEMW it = {0};
    it.mask = LVIF_TEXT;
    it.iItem = idx;

    const wchar_t *name = DeviceTypeNameW(g_devices[idx].d_type);
    wsprintfW(text, L"%s (%u)", name, g_devices[idx].d_type); it.iSubItem = 0; it.pszText = text; ListView_SetItem(g_hDevices, &it);
    wsprintfW(text, L"%u", g_devices[idx].h_adr); it.iSubItem = 1; it.pszText = text; ListView_SetItem(g_hDevices, &it);
    wsprintfW(text, L"%u", g_devices[idx].l_adr); it.iSubItem = 2; it.pszText = text; ListView_SetItem(g_hDevices, &it);
    wsprintfW(text, L"%u", g_devices[idx].zone);  it.iSubItem = 3; it.pszText = text; ListView_SetItem(g_hDevices, &it);
    if (g_devices[idx].version_valid) wsprintfW(text, L"%u", g_devices[idx].version);
    else wsprintfW(text, L"...");
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

static int IsMcuType(uint8_t d_type) {
    return (d_type == 13 || d_type == 14 || d_type == 20 || d_type == 21 || d_type == 22 || d_type == 23);
}

static void RequestDeviceVersion(const DeviceInfo *d) {
    if (!d) return;
    uint32_t can_id_req = BuildCanId(d->d_type, d->h_adr, d->l_adr, d->zone, 0);
    uint8_t data[8] = { CMD_GET_VERSION, 0, 0, 0, 0, 0, 0, 0 };
    SendBsuCanPacket(can_id_req, data, BSU_PKT_TYPE_CAN1);
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

            CreateWindowW(L"STATIC", L"Выберите МКУ для обновления:", WS_CHILD | WS_VISIBLE,
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
                if (ctx->snapshot[i].version_valid) {
                    wsprintfW(row, L"%s  h=%u  l=%u  z=%u  ver=%u",
                              nm, ctx->snapshot[i].h_adr, ctx->snapshot[i].l_adr,
                              ctx->snapshot[i].zone, ctx->snapshot[i].version);
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
        MessageBoxW(owner, L"Список устройств пуст. Дождитесь пакетов от МКУ.", L"Выбор цели", MB_ICONWARNING);
        return 0;
    }

    TargetSelectCtx ctx;
    ZeroMemory(&ctx, sizeof(ctx));
    for (int i = 0; i < g_devCount && i < 256; i++) {
        if (!IsMcuType(g_devices[i].d_type)) continue;
        ctx.snapshot[ctx.count++] = g_devices[i];
    }
    if (ctx.count <= 0) {
        MessageBoxW(owner, L"В списке нет МКУ для обновления.", L"Выбор цели", MB_ICONWARNING);
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
        MessageBoxW(g_hwnd, L"Сначала подключитесь к COM.", L"Updater", MB_ICONWARNING);
        return;
    }
    int sent = 0;
    for (int i = 0; i < g_devCount; i++) {
        if (!IsMcuType(g_devices[i].d_type)) continue;
        RequestDeviceVersion(&g_devices[i]);
        sent++;
    }
    if (sent == 0) {
        Logf(L"Нет устройств для запроса версий.\r\n");
    } else {
        Logf(L"Принудительный запрос версий отправлен: %d устройств.\r\n", sent);
    }
}

static void SelectUpdateTarget(void) {
    DeviceInfo d;
    if (SelectTargetDeviceDialog(g_hwnd, &d)) {
        g_selectedTargetDev = d;
        g_selectedTargetValid = 1;
        Logf(L"Цель обновления: %s h=%u l=%u z=%u\r\n",
             DeviceTypeNameW(d.d_type), d.h_adr, d.l_adr, d.zone);
    }
}

static int WaitAckWord(uint32_t timeout_ms) {
    DWORD start = GetTickCount();
    int ok = 0;
    EnterCriticalSection(&g_ackCs);
    while (!g_ack.matched && (GetTickCount() - start < timeout_ms)) {
        SleepConditionVariableCS(&g_ackCv, &g_ackCs, 2);
    }
    ok = g_ack.matched;
    LeaveCriticalSection(&g_ackCs);
    return ok;
}

typedef struct {
    DeviceInfo dev;
    wchar_t file_path[MAX_PATH];
} UpdaterArgs;

static DWORD WINAPI UpdaterThreadProc(LPVOID arg) {
    UpdaterArgs *ua = (UpdaterArgs *)arg;
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
    SendMessageW(g_hProgress, PBM_SETRANGE32, 0, total_words);
    SendMessageW(g_hProgress, PBM_SETPOS, 0, 0);
    Logf(L"Старт обновления: words=%u\r\n", total_words);
    uint32_t last_progress = 0;

    for (uint32_t i = 0; i < total_words; i++) {
        if (InterlockedCompareExchange(&g_updateStop, 0, 0)) {
            Logf(L"Обновление остановлено пользователем.\r\n");
            break;
        }
        uint8_t b0 = 0xFF, b1 = 0xFF, b2 = 0xFF, b3 = 0xFF;
        uint32_t off = i * 4u;
        if (off + 0 < (uint32_t)fsz) b0 = buf[off + 0];
        if (off + 1 < (uint32_t)fsz) b1 = buf[off + 1];
        if (off + 2 < (uint32_t)fsz) b2 = buf[off + 2];
        if (off + 3 < (uint32_t)fsz) b3 = buf[off + 3];
        uint32_t word = ((uint32_t)b3 << 24) | ((uint32_t)b2 << 16) | ((uint32_t)b1 << 8) | b0;

        uint8_t d[8] = {0};
        d[0] = CMD_SET_UPDATE_WORD;
        d[1] = (uint8_t)((i >> 16) & 0xFF);
        d[2] = (uint8_t)((i >> 8) & 0xFF);
        d[3] = (uint8_t)(i & 0xFF);
        d[4] = (uint8_t)((word >> 24) & 0xFF);
        d[5] = (uint8_t)((word >> 16) & 0xFF);
        d[6] = (uint8_t)((word >> 8) & 0xFF);
        d[7] = (uint8_t)(word & 0xFF);

        /* Режим без подтверждений: отправляем слово и идём дальше. */
        if (!SendBsuCanPacket(can_id_req, d, BSU_PKT_TYPE_CAN1)) {
            Logf(L"Ошибка отправки слова %u\r\n", i);
            break;
        }
        if (((i + 1u) - last_progress) >= 64u || (i + 1u) == total_words) {
            /* Не дёргаем UI на каждом слове: это заметно тормозит цикл передачи. */
            PostMessageW(g_hProgress, PBM_SETPOS, i + 1, 0);
            last_progress = i + 1u;
        }
        Sleep(1);
    }

    if (!InterlockedCompareExchange(&g_updateStop, 0, 0)) {
        uint8_t endd[8] = { CMD_UPDATE_TRANSMIT, 0, 0, 0, 0, 0, 0, 0 };
        SendBsuCanPacket(can_id_req, endd, BSU_PKT_TYPE_CAN1);
        Logf(L"Команда update_transmit отправлена.\r\n");
    }

    free(buf);
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
    InterlockedExchange(&g_updateStop, 0);
    InterlockedExchange(&g_updateRunning, 1);
    EnableWindow(g_hStart, FALSE);
    EnableWindow(g_hStop, TRUE);
    g_hUpdaterThread = CreateThread(NULL, 0, UpdaterThreadProc, ua, 0, NULL);
}

static void StopUpdate(void) {
    InterlockedExchange(&g_updateStop, 1);
    if (g_activeUpdateDevValid && g_hSerial != INVALID_HANDLE_VALUE) {
        uint32_t can_id_req = BuildCanId(g_activeUpdateDev.d_type, g_activeUpdateDev.h_adr,
                                         g_activeUpdateDev.l_adr, g_activeUpdateDev.zone, 0);
        uint8_t endd[8] = { CMD_UPDATE_TRANSMIT, 0, 0, 0, 0, 0, 0, 0 };
        SendBsuCanPacket(can_id_req, endd, BSU_PKT_TYPE_CAN1);
        Logf(L"Принудительное завершение: update_transmit отправлен.\r\n");
    }
}

static void BrowseFile(void) {
    OPENFILENAMEW ofn = {0};
    wchar_t file[MAX_PATH] = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Firmware (*.bin)\0*.bin\0All files\0*.*\0";
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

            g_hPort = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 10, 10, 120, 300, hwnd, (HMENU)IDC_COMBO_PORT, NULL, NULL);
            CreateWindowW(L"BUTTON", L"Обновить порты", WS_CHILD | WS_VISIBLE, 140, 10, 110, 24, hwnd, (HMENU)IDC_BTN_REFRESH, NULL, NULL);
            g_hConnect = CreateWindowW(L"BUTTON", L"Подключить", WS_CHILD | WS_VISIBLE, 260, 10, 100, 24, hwnd, (HMENU)IDC_BTN_CONNECT, NULL, NULL);
            CreateWindowW(L"BUTTON", L"Цель обновления...", WS_CHILD | WS_VISIBLE, 370, 10, 140, 24, hwnd, (HMENU)IDC_BTN_SELECT_TARGET, NULL, NULL);
            CreateWindowW(L"BUTTON", L"Прочитать версии", WS_CHILD | WS_VISIBLE, 520, 10, 130, 24, hwnd, (HMENU)IDC_BTN_FORCE_VERSION, NULL, NULL);

            g_hDevices = CreateWindowW(WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | WS_BORDER,
                                       10, 44, w - 20, 180, hwnd, (HMENU)IDC_LIST_DEVICES, NULL, NULL);
            ListView_SetExtendedListViewStyle(g_hDevices, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
            LVCOLUMNW c = {0}; c.mask = LVCF_TEXT | LVCF_WIDTH;
            c.cx = 180; c.pszText = L"Устройство"; ListView_InsertColumn(g_hDevices, 0, &c);
            c.cx = 70; c.pszText = L"h_adr";  ListView_InsertColumn(g_hDevices, 1, &c);
            c.cx = 70; c.pszText = L"l_adr";  ListView_InsertColumn(g_hDevices, 2, &c);
            c.cx = 70; c.pszText = L"zone";   ListView_InsertColumn(g_hDevices, 3, &c);
            c.cx = 100; c.pszText = L"Версия"; ListView_InsertColumn(g_hDevices, 4, &c);

            g_hFile = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                    10, 232, w - 140, 24, hwnd, (HMENU)IDC_EDIT_FILE, NULL, NULL);
            CreateWindowW(L"BUTTON", L"Файл...", WS_CHILD | WS_VISIBLE, w - 120, 232, 110, 24, hwnd, (HMENU)IDC_BTN_BROWSE, NULL, NULL);

            g_hStart = CreateWindowW(L"BUTTON", L"Старт обновления", WS_CHILD | WS_VISIBLE, 10, 264, 150, 28, hwnd, (HMENU)IDC_BTN_START, NULL, NULL);
            g_hStop = CreateWindowW(L"BUTTON", L"Принудительно завершить", WS_CHILD | WS_VISIBLE, 170, 264, 180, 28, hwnd, (HMENU)IDC_BTN_STOP, NULL, NULL);
            EnableWindow(g_hStop, FALSE);

            g_hProgress = CreateWindowW(PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE, 10, 298, w - 20, 20, hwnd, (HMENU)IDC_PROGRESS, NULL, NULL);
            g_hLog = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
                                   10, 324, w - 20, rc.bottom - 334, hwnd, (HMENU)IDC_EDIT_LOG, NULL, NULL);

            RefreshPorts();
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
        case WM_COMMAND: {
            switch (LOWORD(wp)) {
                case IDC_BTN_REFRESH: RefreshPorts(); break;
                case IDC_BTN_BROWSE: BrowseFile(); break;
                case IDC_BTN_CONNECT:
                    if (!InterlockedCompareExchange(&g_connected, 0, 0)) {
                        if (ConnectSerial()) {
                            SetConnectedUi(1);
                            Logf(L"Подключено.\r\n");
                        } else {
                            MessageBoxW(hwnd, L"Ошибка подключения к COM.", L"Updater", MB_ICONERROR);
                        }
                    } else {
                        DisconnectSerial();
                        SetConnectedUi(0);
                        Logf(L"Отключено.\r\n");
                    }
                    break;
                case IDC_BTN_SELECT_TARGET: SelectUpdateTarget(); break;
                case IDC_BTN_FORCE_VERSION: ForceReadVersions(); break;
                case IDC_BTN_START: StartUpdate(); break;
                case IDC_BTN_STOP: StopUpdate(); break;
            }
            break;
        }
        case WM_APP_PACKET: {
            PostedPacket *pp = (PostedPacket *)lp;
            if (!pp) break;
            CanIdFields f = ParseCanId(pp->can_id);
            if (f.dir == 1) {
                if (IsMcuType(f.d_type)) {
                    DeviceInfo d = {0};
                    d.d_type = f.d_type;
                    d.h_adr = f.h_adr;
                    d.l_adr = f.l_adr;
                    d.zone = f.zone;
                    d.last_seen_ms = GetTickCount();
                    int idx = FindOrAddDevice(d);
                    if (idx >= 0) {
                        uint8_t need_version_request = 0;
                        if (g_devices[idx].d_type == 0) g_devices[idx] = d;
                        else {
                            g_devices[idx].d_type = d.d_type;
                            g_devices[idx].h_adr = d.h_adr;
                            g_devices[idx].l_adr = d.l_adr;
                            g_devices[idx].zone = d.zone;
                            g_devices[idx].last_seen_ms = d.last_seen_ms;
                        }
                        if (!g_devices[idx].version_valid) need_version_request = 1;
                        if (idx >= ListView_GetItemCount(g_hDevices)) AddDeviceToList(idx);
                        else RefreshDeviceListRow(idx);
                        if (need_version_request) RequestDeviceVersion(&g_devices[idx]);
                    }
                }
            }
            if (f.dir == 1 && pp->data[0] == CMD_GET_VERSION) {
                uint32_t ver = ((uint32_t)pp->data[1] << 24) |
                               ((uint32_t)pp->data[2] << 16) |
                               ((uint32_t)pp->data[3] << 8) |
                               (uint32_t)pp->data[4];
                for (int i = 0; i < g_devCount; i++) {
                    if (g_devices[i].d_type == f.d_type &&
                        g_devices[i].h_adr == f.h_adr &&
                        g_devices[i].l_adr == f.l_adr &&
                        g_devices[i].zone == f.zone) {
                        g_devices[i].version = ver;
                        g_devices[i].version_valid = 1;
                        RefreshDeviceListRow(i);
                        break;
                    }
                }
            }
            free(pp);
            break;
        }
        case WM_APP_UPD_DONE:
            if (g_hUpdaterThread) {
                CloseHandle(g_hUpdaterThread);
                g_hUpdaterThread = NULL;
            }
            EnableWindow(g_hStart, TRUE);
            EnableWindow(g_hStop, FALSE);
            g_activeUpdateDevValid = 0;
            Logf(L"Обновление завершено.\r\n");
            break;
        case WM_DESTROY:
            StopUpdate();
            if (g_hUpdaterThread) {
                WaitForSingleObject(g_hUpdaterThread, 1000);
                CloseHandle(g_hUpdaterThread);
                g_hUpdaterThread = NULL;
            }
            DisconnectSerial();
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
                              CW_USEDEFAULT, CW_USEDEFAULT, 840, 640, NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

