#pragma execution_character_set("utf-8")
#define WIN32_LEAN_AND_MEAN
#include "..\Common.h"
#include <windows.h>
#include <string>
#include <vector>
#include <stdint.h>
#include <time.h>
#include <commctrl.h>
#pragma comment(lib, "comctl32")

#define SERVER_IP   "10.10.108.105"
#define SERVER_PORT  9000

typedef enum {
    PT_SENSOR_DATA = 0x01, PT_CHAT_MSG = 0x02, PT_SOS_ALERT = 0x03,
    PT_CMD_CONTROL = 0x04, PT_DB_QUERY = 0x05, PT_EMERGENCY = 0x06
} PacketType;
#define PKT_PREAMBLE 0xAA
#pragma pack(push,1)
typedef struct { uint8_t sensor_type; float value; } SensorData;
typedef struct { char message[128]; } ChatData;
typedef struct { uint8_t device_id; uint8_t action; } ControlData;
typedef struct { uint8_t event_type; float value; } EmergencyData;
typedef struct {
    uint8_t  preamble;
    uint8_t  type;
    char     sender_id[16];
    union { SensorData sensor; ChatData chat; ControlData ctrl; EmergencyData emergency; } payload;
    uint32_t timestamp;
} NetPacket;
#pragma pack(pop)

// Control IDs
#define ID_LOG       101
#define ID_INPUT     102
#define ID_SEND      103
#define ID_HISTORY   104
#define ID_CLEAR     105
#define ID_NOTICE    106
#define ID_CARD_TEMP 110
#define ID_CARD_HUMI 111
#define ID_CARD_PIR  112
#define ID_CARD_FALL 113
#define ID_UPDATE    114   // 카드 업데이트 버튼

// Custom messages
#define WM_APPENDLOG    (WM_USER+1)
#define WM_SETALERT     (WM_USER+2)
#define WM_SETPOPUP     (WM_USER+3)
#define WM_UPDATESENSOR (WM_USER+4)
#define WM_SHOWHISTORY  (WM_USER+5)

// Timers
#define TIMER_FLASH  1
#define TIMER_BEEP   2
#define TIMER_CLOCK  3
#define FLASH_MS     500
#define BEEP_MS      800
#define CLOCK_MS     1000

// ---------------------------------------------------------------
// Globals
// ---------------------------------------------------------------
HWND   g_hWnd = nullptr;
HWND   g_hLog = nullptr;
HWND   g_hInput = nullptr;
HWND   g_hCardTemp = nullptr;
HWND   g_hCardHumi = nullptr;
HWND   g_hCardPir = nullptr;
HWND   g_hCardFall = nullptr;
HWND   g_hClock = nullptr;
SOCKET g_sock = INVALID_SOCKET;

bool         g_alert = false;
bool         g_flashOn = false;
std::wstring g_alertMsg;

HBRUSH g_hRedBrush1 = nullptr;
HBRUSH g_hRedBrush2 = nullptr;
HBRUSH g_hNormalBrush = nullptr;
HBRUSH g_hTitleBrush = nullptr;
HFONT  g_hPopupFont = nullptr;
HFONT  g_hClockFont = nullptr;
HFONT  g_hCardFont = nullptr;
HFONT  g_hCardValFont = nullptr;
HFONT  g_hBtnFont = nullptr;

// Sensor values
std::wstring g_valTemp = L"-- C";
std::wstring g_valHumi = L"-- %";
std::wstring g_valPir = L"--";
std::wstring g_valFall = L"\uC548\uC804";  // 안전
COLORREF     g_fallClr = RGB(44, 44, 42);

// History
struct HistRow { std::wstring date, type, detail, status; };
std::vector<HistRow> g_history;
bool g_histReady = false;

WNDPROC g_origCard = nullptr;

// ---------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

// pipe-separated string split (no lambda)
std::vector<std::string> splitPipe(const std::string& s) {
    std::vector<std::string> v;
    size_t p = 0, q;
    while ((q = s.find('|', p)) != std::string::npos) {
        v.push_back(s.substr(p, q - p));
        p = q + 1;
    }
    v.push_back(s.substr(p));
    return v;
}

void AppendLogW(const std::wstring& msg) {
    std::wstring* p = new std::wstring(msg);
    PostMessage(g_hWnd, WM_APPENDLOG, 0, (LPARAM)p);
}
void AppendLog(const std::string& msg) { AppendLogW(Utf8ToWide(msg)); }

// ---------------------------------------------------------------
// Alert flash
// ---------------------------------------------------------------
void SetAlert(bool on) {
    g_alert = on; g_flashOn = on;
    if (on) {
        SetTimer(g_hWnd, TIMER_FLASH, FLASH_MS, nullptr);
        SetTimer(g_hWnd, TIMER_BEEP, BEEP_MS, nullptr);
        MessageBeep(MB_ICONHAND);
        RECT r = { 0,0,710,390 };
        RedrawWindow(g_hWnd, &r, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_NOCHILDREN);
    }
    else {
        KillTimer(g_hWnd, TIMER_FLASH);
        KillTimer(g_hWnd, TIMER_BEEP);
        g_alertMsg.clear();
        RedrawWindow(g_hWnd, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
}
void RefreshCards() {
    InvalidateRect(g_hCardTemp, nullptr, TRUE);
    InvalidateRect(g_hCardHumi, nullptr, TRUE);
    InvalidateRect(g_hCardPir, nullptr, TRUE);
    InvalidateRect(g_hCardFall, nullptr, TRUE);
}

// ---------------------------------------------------------------
// Network
// ---------------------------------------------------------------
void sendPacket(NetPacket& pkt) {
    pkt.preamble = PKT_PREAMBLE;
    strncpy(pkt.sender_id, "GUARDIAN", 15);
    pkt.timestamp = (uint32_t)time(nullptr);
    send(g_sock, (char*)&pkt, sizeof(pkt), 0);
}
void sendChat(const std::string& msg) {
    NetPacket pkt; memset(&pkt, 0, sizeof(pkt));
    pkt.type = PT_CHAT_MSG;
    strncpy(pkt.payload.chat.message, msg.c_str(), 127);
    sendPacket(pkt);
}

// ---------------------------------------------------------------
// Recv thread
// ---------------------------------------------------------------
DWORD WINAPI RecvThread(LPVOID) {
    NetPacket pkt;
    while (true) {
        int n = recv(g_sock, (char*)&pkt, sizeof(pkt), MSG_WAITALL);
        if (n <= 0) break;

        if (pkt.type != PT_CHAT_MSG) continue;
        std::string msg(pkt.payload.chat.message);
        std::string sender(pkt.sender_id);

        // CARD: 센서카드 전용 업데이트 (로그창 출력 안 함)
        if (msg.rfind("CARD:", 0) == 0) {
            std::string body = msg.substr(5); // "TEMP:23.9" 등
            if (body.rfind("TEMP:", 0) == 0) {
                float v = (float)atof(body.substr(5).c_str());
                wchar_t buf[32]; swprintf(buf, 32, L"%.1f C", v);
                g_valTemp = buf;
            }
            else if (body.rfind("HUMI:", 0) == 0) {
                float v = (float)atof(body.substr(5).c_str());
                wchar_t buf[32]; swprintf(buf, 32, L"%.1f %%", v);
                g_valHumi = buf;
            }
            else if (body.rfind("PIR:", 0) == 0) {
                float v = (float)atof(body.substr(4).c_str());
                g_valPir = (v == 1.0f)
                    ? L"감지됨"   // 감지됨
                    : L"없음";         // 없음
            }
            PostMessage(g_hWnd, WM_UPDATESENSOR, 0, 0);
            continue;
        }

        // History protocol
        if (msg == "HISTORY_START") {
            g_history.clear();
            g_histReady = false;
            continue;
        }
        if (msg.rfind("HISTORY|", 0) == 0) {
            std::vector<std::string> parts = splitPipe(msg.substr(8));
            if (parts.size() >= 4) {
                HistRow r;
                r.date = Utf8ToWide(parts[0]);
                r.type = Utf8ToWide(parts[1]);
                r.detail = Utf8ToWide(parts[2]);
                r.status = Utf8ToWide(parts[3]);
                g_history.push_back(r);
            }
            continue;
        }
        if (msg == "HISTORY_END") {
            g_histReady = true;
            PostMessage(g_hWnd, WM_SHOWHISTORY, 0, 0);
            continue;
        }

        // Ignore "Message sent"
        if (msg.find("Message sent") != std::string::npos) continue;

        // Emergency alerts -> flash
        if (msg.find("[ALERT]") != std::string::npos ||
            msg.find("[!!!]") != std::string::npos) {
            AppendLog(msg);
            std::wstring* reason = new std::wstring(Utf8ToWide(msg));
            PostMessage(g_hWnd, WM_SETPOPUP, 0, (LPARAM)reason);
            PostMessage(g_hWnd, WM_SETALERT, 1, 0);
            // fall card
            if (msg.find("\xEB\x82\x99\xEC\x83\x81") != std::string::npos) { // 낙상
                g_valFall = L"\uACBD\uBCF4! \uC989\uC2DC \uD655\uC778"; // 경보! 즉시 확인
                g_fallClr = RGB(163, 45, 45);
                PostMessage(g_hWnd, WM_UPDATESENSOR, 0, 0);
            }
        }
        // Warning -> log only
        else if (msg.find("[WARN]") != std::string::npos) {
            AppendLog(msg);
            if (msg.find("\xEB\x82\x99\xEC\x83\x81") != std::string::npos) { // 낙상
                g_valFall = L"\uC8FC\uC758 (30\uCD08 \uC9C0\uC18D)"; // 주의 (30초 지속)
                g_fallClr = RGB(186, 117, 23);
                PostMessage(g_hWnd, WM_UPDATESENSOR, 0, 0);
            }
        }
        // Senior chat
        else if (sender == "SENIOR") {
            AppendLog("거주자 : " + msg); // 거주자 :
        }
        // Others (server messages) - STATUS 형식 메시지 로그 제외
        else {
            // 센서 상태값 형식 메시지는 로그에 출력 안 함
            if (msg.find("=== STATUS") == std::string::npos &&
                msg.find("==============") == std::string::npos &&
                msg.find("[INFO]") == std::string::npos) {
                AppendLog(msg);
            }
        }


    }
    AppendLogW(L"\uC11C\uBC84 \uC5F0\uACB0 \uC885\uB8CC"); // 서버 연결 종료
    return 0;
}

bool ConnectServer() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock == INVALID_SOCKET) return false;
    sockaddr_in addr;  memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    addr.sin_addr.s_addr = inet_addr(SERVER_IP);
    if (connect(g_sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) return false;
    sendChat("GUARDIAN");
    CreateThread(nullptr, 0, RecvThread, nullptr, 0, nullptr);
    return true;
}

void SendCardUpdate() {
    if (g_sock == INVALID_SOCKET) return;
    NetPacket pkt; memset(&pkt, 0, sizeof(pkt));
    pkt.type = PT_DB_QUERY;
    strncpy(pkt.payload.chat.message, "CARD", 127);
    sendPacket(pkt);
}

void SendHistory() {
    if (g_sock == INVALID_SOCKET) return;
    NetPacket pkt; memset(&pkt, 0, sizeof(pkt));
    pkt.type = PT_DB_QUERY;
    // payload 비워서 전송 -> 서버에서 history로 처리
    pkt.payload.chat.message[0] = ' ';
    sendPacket(pkt);
}

void SendChat() {
    wchar_t buf[256] = { 0 };
    GetWindowTextW(g_hInput, buf, 256);
    if (!wcslen(buf)) return;
    sendChat(WideToUtf8(buf));
    AppendLogW(std::wstring(L"\uB098(\uBCF4\uD638\uC790) : ") + buf); // 나(보호자) :
    SetWindowTextW(g_hInput, L""); SetFocus(g_hInput);
}
void SendNotice() {
    wchar_t buf[256] = { 0 };
    GetWindowTextW(g_hInput, buf, 256);
    if (!wcslen(buf)) return;
    sendChat("NOTICE:" + WideToUtf8(buf));
    AppendLogW(std::wstring(L"[\uC54C\uB9BC\uD310] ") + buf); // [알림판]
    SetWindowTextW(g_hInput, L""); SetFocus(g_hInput);
}

// ---------------------------------------------------------------
// Sensor card subclass
// ---------------------------------------------------------------
LRESULT CALLBACK CardProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc; GetClientRect(hWnd, &rc);

        COLORREF bg, fc;
        const wchar_t* label, * val;
        if (hWnd == g_hCardTemp) {
            bg = RGB(230, 241, 251); fc = RGB(12, 68, 124);
            label = L"\uC628\uB3C4"; val = g_valTemp.c_str(); // 온도
        }
        else if (hWnd == g_hCardHumi) {
            bg = RGB(225, 245, 238); fc = RGB(8, 80, 65);
            label = L"\uC2B5\uB3C4"; val = g_valHumi.c_str(); // 습도
        }
        else if (hWnd == g_hCardPir) {
            bg = RGB(234, 243, 222); fc = RGB(39, 80, 10);
            label = L"\uC6C0\uC9C1\uC784"; val = g_valPir.c_str(); // 움직임
        }
        else {
            bg = RGB(241, 239, 232); fc = g_fallClr;
            label = L"\uB099\uC0C1\uC704\uD5D8"; val = g_valFall.c_str(); // 낙상위험
        }

        HBRUSH hbr = CreateSolidBrush(bg);
        FillRect(hdc, &rc, hbr); DeleteObject(hbr);
        SetBkMode(hdc, TRANSPARENT); SetTextColor(hdc, fc);

        HFONT hOld = (HFONT)SelectObject(hdc, g_hCardFont);
        RECT rL = { rc.left + 8, rc.top + 6, rc.right - 8, rc.top + 22 };
        DrawTextW(hdc, label, -1, &rL, DT_LEFT | DT_SINGLELINE);

        SelectObject(hdc, g_hCardValFont);
        RECT rV = { rc.left + 4, rc.top + 22, rc.right - 4, rc.bottom - 4 };
        DrawTextW(hdc, val, -1, &rV, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        SelectObject(hdc, hOld);
        EndPaint(hWnd, &ps); return 0;
    }
    return CallWindowProc(g_origCard, hWnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------
// History dialog (simple modal)
// ---------------------------------------------------------------
LRESULT CALLBACK HistDlgProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_COMMAND && LOWORD(wParam) == IDOK) {
        DestroyWindow(hWnd); return 0;
    }
    if (msg == WM_CLOSE) { DestroyWindow(hWnd); return 0; }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

void ShowHistoryDialog() {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc; memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = HistDlgProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = L"HistDlg";
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }

    HWND hDlg = CreateWindowW(L"HistDlg",
        L"\uC704\uD5D8 \uAE30\uB85D \uC870\uD68C",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        80, 50, 820, 500, g_hWnd, nullptr, GetModuleHandle(nullptr), nullptr);

    // ListView - column-based table (handles Korean width correctly)
    HWND hList = CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        8, 8, 786, 390, hDlg, nullptr, GetModuleHandle(nullptr), nullptr);
    ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    HFONT hLF = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Arial");
    SendMessage(hList, WM_SETFONT, (WPARAM)hLF, TRUE);

    // Columns
    LVCOLUMNW col; memset(&col, 0, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    col.fmt = LVCFMT_LEFT;

    col.cx = 110; col.pszText = (LPWSTR)L"\uBC1C\uC0DD \uC77C\uC2DC";
    ListView_InsertColumn(hList, 0, &col);
    col.cx = 70;  col.pszText = (LPWSTR)L"\uAD6C\uBD84";
    ListView_InsertColumn(hList, 1, &col);
    col.cx = 430; col.pszText = (LPWSTR)L"\uC0C1\uC138 \uB0B4\uC6A9";
    ListView_InsertColumn(hList, 2, &col);
    col.cx = 110; col.pszText = (LPWSTR)L"\uC870\uCE58 \uC0C1\uD0DC";
    ListView_InsertColumn(hList, 3, &col);

    // Rows
    if (g_history.empty()) {
        LVITEMW item; memset(&item, 0, sizeof(item));
        item.mask = LVIF_TEXT; item.iItem = 0;
        item.pszText = (LPWSTR)L"--";
        ListView_InsertItem(hList, &item);
        ListView_SetItemText(hList, 0, 1, (LPWSTR)L"--");
        ListView_SetItemText(hList, 0, 2, (LPWSTR)L"\uC704\uD5D8 \uAE30\uB85D\uC774 \uC5C6\uC2B5\uB2C8\uB2E4.");
        ListView_SetItemText(hList, 0, 3, (LPWSTR)L"--");
    }
    else {
        for (int i = 0; i < (int)g_history.size(); i++) {
            HistRow& r = g_history[i];
            LVITEMW item; memset(&item, 0, sizeof(item));
            item.mask = LVIF_TEXT;
            item.iItem = i;
            item.pszText = (LPWSTR)r.date.c_str();
            ListView_InsertItem(hList, &item);
            ListView_SetItemText(hList, i, 1, (LPWSTR)r.type.c_str());
            ListView_SetItemText(hList, i, 2, (LPWSTR)r.detail.c_str());
            ListView_SetItemText(hList, i, 3, (LPWSTR)r.status.c_str());
        }
    }

    HWND hOK = CreateWindowW(L"BUTTON", L"\uB2EB\uAE30",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        360, 406, 100, 28, hDlg, (HMENU)IDOK, nullptr, nullptr);
    SendMessage(hOK, WM_SETFONT, (WPARAM)g_hBtnFont, TRUE);

    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);
}

// ---------------------------------------------------------------
// Main WndProc
// ---------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_hRedBrush1 = CreateSolidBrush(RGB(210, 30, 30));
        g_hRedBrush2 = CreateSolidBrush(RGB(140, 10, 10));
        g_hNormalBrush = CreateSolidBrush(RGB(245, 245, 245));
        g_hTitleBrush = CreateSolidBrush(RGB(44, 44, 42));

        g_hPopupFont = CreateFontW(32, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Arial");
        g_hClockFont = CreateFontW(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Arial");
        g_hCardFont = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Arial");
        g_hCardValFont = CreateFontW(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Arial");
        g_hBtnFont = CreateFontW(15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Arial");

        // 업데이트 버튼 (좌상단)
        HWND hUpd = CreateWindowW(L"BUTTON", L"⟳ 업데이트", // ⟳ 업데이트
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            598, 24, 92, 20, hWnd, (HMENU)ID_UPDATE, nullptr, nullptr);
        HFONT hUF = CreateFontW(12, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Arial");
        SendMessage(hUpd, WM_SETFONT, (WPARAM)hUF, TRUE);

        // Clock bar (y=0)
        g_hClock = CreateWindowW(L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            0, 0, 710, 22, hWnd, nullptr, nullptr, nullptr);
        SendMessage(g_hClock, WM_SETFONT, (WPARAM)g_hClockFont, TRUE);
        SetTimer(hWnd, TIMER_CLOCK, CLOCK_MS, nullptr);

        // Sensor cards (y=28)
        g_hCardTemp = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_NOTIFY,
            8, 46, 160, 56, hWnd, (HMENU)ID_CARD_TEMP, nullptr, nullptr);
        g_hCardHumi = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_NOTIFY,
            176, 46, 160, 56, hWnd, (HMENU)ID_CARD_HUMI, nullptr, nullptr);
        g_hCardPir = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_NOTIFY,
            344, 46, 160, 56, hWnd, (HMENU)ID_CARD_PIR, nullptr, nullptr);
        g_hCardFall = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_NOTIFY,
            512, 46, 178, 56, hWnd, (HMENU)ID_CARD_FALL, nullptr, nullptr);

        g_origCard = (WNDPROC)SetWindowLongPtr(g_hCardTemp, GWLP_WNDPROC, (LONG_PTR)CardProc);
        SetWindowLongPtr(g_hCardHumi, GWLP_WNDPROC, (LONG_PTR)CardProc);
        SetWindowLongPtr(g_hCardPir, GWLP_WNDPROC, (LONG_PTR)CardProc);
        SetWindowLongPtr(g_hCardFall, GWLP_WNDPROC, (LONG_PTR)CardProc);

        // Log (y=90)
        g_hLog = CreateWindowW(L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            8, 108, 674, 302, hWnd, (HMENU)ID_LOG, nullptr, nullptr);
        SendMessage(g_hLog, EM_SETLIMITTEXT, 300000, 0);

        // Input (y=418)
        g_hInput = CreateWindowW(L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            8, 418, 380, 30, hWnd, (HMENU)ID_INPUT, nullptr, nullptr);

        // Buttons
        HWND h;
        h = CreateWindowW(L"BUTTON", L"\uC804\uC1A1", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, // 전송
            396, 418, 60, 30, hWnd, (HMENU)ID_SEND, nullptr, nullptr);
        SendMessage(h, WM_SETFONT, (WPARAM)g_hBtnFont, TRUE);

        h = CreateWindowW(L"BUTTON", L"\uC54C\uB9BC\uD310", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, // 알림판
            464, 418, 64, 30, hWnd, (HMENU)ID_NOTICE, nullptr, nullptr);
        SendMessage(h, WM_SETFONT, (WPARAM)g_hBtnFont, TRUE);

        h = CreateWindowW(L"BUTTON", L"\uAE30\uB85D\uC870\uD68C", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, // 기록조회
            536, 418, 76, 30, hWnd, (HMENU)ID_HISTORY, nullptr, nullptr);
        SendMessage(h, WM_SETFONT, (WPARAM)g_hBtnFont, TRUE);

        h = CreateWindowW(L"BUTTON", L"\uACBD\uACE0 \uD574\uC81C", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, // 경고 해제
            8, 456, 120, 32, hWnd, (HMENU)ID_CLEAR, nullptr, nullptr);
        SendMessage(h, WM_SETFONT, (WPARAM)g_hBtnFont, TRUE);

        // Connect
        AppendLogW(L"=== \uBCF4\uD638\uC790 \uB300\uC2DC\uBCF4\uB4DC \uC2DC\uC791 ==="); // 보호자 대시보드 시작
        if (ConnectServer()) {
            AppendLogW(L"[\uC5F0\uACB0 \uC131\uACF5]"); // [연결 성공]
            SendCardUpdate();
        }
        break;
    }

    case WM_APPENDLOG: {
        std::wstring* p = (std::wstring*)lParam;
        if (p) {
            int len = GetWindowTextLength(g_hLog);
            SendMessage(g_hLog, EM_SETSEL, len, len);
            SendMessage(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)p->c_str());
            SendMessage(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
            SendMessage(g_hLog, WM_VSCROLL, SB_BOTTOM, 0);
            delete p;
        }
        break;
    }

    case WM_SETPOPUP: {
        std::wstring* p = (std::wstring*)lParam;
        if (p) { g_alertMsg = *p; delete p; }
        break;
    }

    case WM_SETALERT: SetAlert(wParam == 1); break;
    case WM_UPDATESENSOR: RefreshCards(); break;

    case WM_SHOWHISTORY:
        ShowHistoryDialog();
        break;

    case WM_TIMER: {
        if (wParam == TIMER_FLASH && g_alert) {
            g_flashOn = !g_flashOn;
            RECT r = { 0,0,710,390 };
            RedrawWindow(hWnd, &r, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_NOCHILDREN);
        }
        if (wParam == TIMER_BEEP && g_alert) MessageBeep(MB_ICONHAND);
        if (wParam == TIMER_CLOCK) {
            SYSTEMTIME st; GetLocalTime(&st);
            wchar_t buf[64];
            swprintf(buf, 64, L"%04d-%02d-%02d  %02d:%02d:%02d",
                st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond);
            HDC hdc = GetDC(g_hClock);
            RECT rc; GetClientRect(g_hClock, &rc);
            HBRUSH bg = g_alert ? (g_flashOn ? g_hRedBrush1 : g_hRedBrush2) : g_hTitleBrush;
            FillRect(hdc, &rc, bg);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(210, 210, 200));
            HFONT hOld = (HFONT)SelectObject(hdc, g_hClockFont);
            DrawTextW(hdc, buf, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, hOld);
            ReleaseDC(g_hClock, hdc);
        }
        break;
    }

    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rc; GetClientRect(hWnd, &rc);
        FillRect(hdc, &rc, g_hNormalBrush);
        RECT tr = { 0,0,rc.right,22 };
        FillRect(hdc, &tr, g_hTitleBrush);
        if (g_alert) {
            RECT ar = { 0,22,rc.right,390 };
            FillRect(hdc, &ar, g_flashOn ? g_hRedBrush1 : g_hRedBrush2);
            if (!g_alertMsg.empty()) {
                HFONT hOld = (HFONT)SelectObject(hdc, g_hPopupFont);
                SetTextColor(hdc, RGB(255, 255, 255));
                SetBkMode(hdc, TRANSPARENT);
                RECT tr2 = { 10,200,680,380 };
                DrawTextW(hdc, g_alertMsg.c_str(), -1, &tr2,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                SelectObject(hdc, hOld);
            }
        }
        return 1;
    }

    case WM_COMMAND: {
        switch (LOWORD(wParam)) {
        case ID_UPDATE:  SendCardUpdate(); break;
        case ID_SEND:    SendChat();   break;
        case ID_NOTICE:  SendNotice(); break;
        case ID_HISTORY: {
            SendHistory();
            break;
        }
        case ID_CLEAR:
            SetAlert(false);
            AppendLogW(L"[\uACBD\uACE0 \uD574\uC81C \uC644\uB8CC]"); // [경고 해제 완료]
            break;
        }
        break;
    }

    case WM_KEYDOWN:
        if (wParam == VK_RETURN && GetFocus() == g_hInput) SendChat();
        break;

    case WM_DESTROY:
        KillTimer(hWnd, TIMER_FLASH);
        KillTimer(hWnd, TIMER_BEEP);
        KillTimer(hWnd, TIMER_CLOCK);
        if (g_sock != INVALID_SOCKET) { closesocket(g_sock); WSACleanup(); }
        if (g_hRedBrush1)   DeleteObject(g_hRedBrush1);
        if (g_hRedBrush2)   DeleteObject(g_hRedBrush2);
        if (g_hNormalBrush) DeleteObject(g_hNormalBrush);
        if (g_hTitleBrush)  DeleteObject(g_hTitleBrush);
        if (g_hPopupFont)   DeleteObject(g_hPopupFont);
        if (g_hClockFont)   DeleteObject(g_hClockFont);
        if (g_hCardFont)    DeleteObject(g_hCardFont);
        if (g_hCardValFont) DeleteObject(g_hCardValFont);
        if (g_hBtnFont)     DeleteObject(g_hBtnFont);
        PostQuitMessage(0);
        break;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int main() {
    HINSTANCE hInst = GetModuleHandle(nullptr);
    WNDCLASSW wc;  memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"GuardianUI";
    RegisterClassW(&wc);

    g_hWnd = CreateWindowW(L"GuardianUI",
        L"\uBCF4\uD638\uC790 \uB300\uC2DC\uBCF4\uB4DC - Smart Home", // 보호자 대시보드
        WS_OVERLAPPEDWINDOW, 100, 100, 710, 530,
        nullptr, nullptr, hInst, nullptr);
    ShowWindow(g_hWnd, SW_SHOW);
    UpdateWindow(g_hWnd);

    MSG message;
    while (GetMessage(&message, nullptr, 0, 0)) {
        TranslateMessage(&message);
        DispatchMessage(&message);
    }
    return (int)message.wParam;
}