#pragma execution_character_set("utf-8")
#define WIN32_LEAN_AND_MEAN
#include "..\Common.h"
#include <windows.h>
#include <string>
#include <stdint.h>
#include <time.h>

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

// ---------------------------------------------------------------
// Control IDs
// ---------------------------------------------------------------
#define ID_NOTICE   201
#define ID_LOG      202
#define ID_INPUT    203
#define ID_SEND     204
#define ID_SOS      205

// Custom messages
#define WM_APPENDLOG    (WM_USER+1)
#define WM_UPDATENOTICE (WM_USER+2)

// Timers
#define TIMER_SOS    1
#define TIMER_CLOCK  2
#define SOS_HOLD_MS  3000
#define CLOCK_MS     1000

// Colors (GuardianUI 계열 통일)
#define CLR_TITLEBAR  RGB(44,44,42)
#define CLR_BG        RGB(245,245,245)
#define CLR_NOTICE_BG RGB(255,224, 102)    // 알림판 노란색
#define CLR_SOS_NORMAL RGB(210,30,30)    // SOS 버튼 빨간색
#define CLR_SOS_PRESS  RGB(140,10,10)

// ---------------------------------------------------------------
// Globals
// ---------------------------------------------------------------
HWND   g_hWnd = nullptr;
HWND   g_hNotice = nullptr;
HWND   g_hLog = nullptr;
HWND   g_hInput = nullptr;
HWND   g_hClock = nullptr;
HWND   g_hSosBtn = nullptr;
SOCKET g_sock = INVALID_SOCKET;

HBRUSH g_hTitleBrush = nullptr;
HBRUSH g_hBgBrush = nullptr;
HBRUSH g_hYellowBrush = nullptr;
HBRUSH g_hSosBrush = nullptr;

HFONT  g_hClockFont = nullptr;
HFONT  g_hNoticeFont = nullptr;
HFONT  g_hLogFont = nullptr;
HFONT  g_hBtnFont = nullptr;
HFONT  g_hSosFont = nullptr;

bool   g_sosHolding = false;

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
void AppendLogW(const std::wstring& msg) {
    std::wstring* p = new std::wstring(msg);
    PostMessage(g_hWnd, WM_APPENDLOG, 0, (LPARAM)p);
}
void AppendLog(const std::string& msg) { AppendLogW(Utf8ToWide(msg)); }

void UpdateNotice(const std::string& msg) {
    std::wstring text = Utf8ToWide(msg);
    if (text.size() > 30) text = text.substr(0, 30) + L"...";
    std::wstring* p = new std::wstring(text);
    PostMessage(g_hWnd, WM_UPDATENOTICE, 0, (LPARAM)p);
}

// ---------------------------------------------------------------
// Network
// ---------------------------------------------------------------
void sendPacket(NetPacket& pkt) {
    pkt.preamble = PKT_PREAMBLE;
    strncpy(pkt.sender_id, "SENIOR", 15);
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

        // Message sent 무시
        if (msg.find("Message sent") != std::string::npos) continue;

        // NOTICE: 접두사 → 알림판만 업데이트
        if (msg.rfind("NOTICE:", 0) == 0) {
            UpdateNotice(msg.substr(7));
            MessageBeep(MB_ICONASTERISK);
        }
        // 긴급 알림 → 알림판 + 로그
        else if (msg.find("[ALERT]") != std::string::npos ||
            msg.find("[!!!]") != std::string::npos) {
            AppendLog(msg);
            UpdateNotice(msg);
            MessageBeep(MB_ICONHAND);
        }
        // 경고 → 알림판 + 로그
        else if (msg.find("[WARN]") != std::string::npos) {
            AppendLog(msg);
            UpdateNotice(msg);
            MessageBeep(MB_ICONASTERISK);
        }
        // SOS 확인 → 알림판
        else if (msg.find("SOS sent") != std::string::npos) {
            AppendLog(msg);
            UpdateNotice("SOS sent to guardian!");
        }
        // 보호자 채팅 → 로그만
        else if (sender == "GUARDIAN") {
            AppendLog("\xEB\xB3\xB4\xED\x98\xB8\xEC\x9E\x90 : " + msg); // 보호자 :
        }
        else {
            AppendLog(msg);
        }
    }
    AppendLogW(L"\xC11C\xBC84 \xC5F0\xACB0 \xC885\xB8CC"); // 서버 연결 종료
    return 0;
}

bool ConnectServer() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    g_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (g_sock == INVALID_SOCKET) return false;
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    addr.sin_addr.s_addr = inet_addr(SERVER_IP);
    if (connect(g_sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) return false;
    sendChat("SENIOR");
    CreateThread(nullptr, 0, RecvThread, nullptr, 0, nullptr);
    return true;
}

// ---------------------------------------------------------------
// Chat / SOS
// ---------------------------------------------------------------
void SendChat() {
    wchar_t buf[256] = {};
    GetWindowTextW(g_hInput, buf, 256);
    if (!wcslen(buf)) return;
    sendChat(WideToUtf8(buf));
    AppendLogW(std::wstring(L"\xB098(\xAC70\xC8FC\xC790) : ") + buf); // 나(거주자) :
    SetWindowTextW(g_hInput, L""); SetFocus(g_hInput);
}

void SendSOS() {
    if (g_sock == INVALID_SOCKET) return;
    NetPacket pkt; memset(&pkt, 0, sizeof(pkt));
    pkt.type = PT_SOS_ALERT;
    sendPacket(pkt);
    UpdateNotice("SOS sent to guardian!");
    AppendLogW(L"[SOS] \xAE34\xAE09 \xD638\xCD9C \xBC1C\xC1A1!"); // 긴급 호출 발송!
    MessageBeep(MB_ICONHAND);
}

// ---------------------------------------------------------------
// WndProc
// ---------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_hTitleBrush = CreateSolidBrush(CLR_TITLEBAR);
        g_hBgBrush = CreateSolidBrush(CLR_BG);
        g_hYellowBrush = CreateSolidBrush(CLR_NOTICE_BG);
        g_hSosBrush = CreateSolidBrush(CLR_SOS_NORMAL);

        g_hClockFont = CreateFontW(14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Arial");
        g_hNoticeFont = CreateFontW(28, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Arial");
        g_hLogFont = CreateFontW(25, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Arial");
        g_hBtnFont = CreateFontW(15, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Arial");
        g_hSosFont = CreateFontW(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Arial");

        // 시계 (타이틀바)
        g_hClock = CreateWindowW(L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_CENTER,
            0, 0, 700, 22, hWnd, nullptr, nullptr, nullptr);
        SendMessage(g_hClock, WM_SETFONT, (WPARAM)g_hClockFont, TRUE);
        SetTimer(hWnd, TIMER_CLOCK, CLOCK_MS, nullptr);

        // 알림판 (y=28) - 보호자 메시지/긴급알림 표시
        g_hNotice = CreateWindowW(L"STATIC",
            L"\uBCF4\uD638\uC790\uC758 \uBA54\uC2DC\uC9C0\uB97C \uAE30\uB2E4\uB9AC\uB294 \uC911...", // 보호자의 메시지를 기다리는 중...
            WS_CHILD | WS_VISIBLE | WS_BORDER | SS_CENTER | SS_CENTERIMAGE | SS_ENDELLIPSIS,
            8, 28, 684, 60, hWnd, (HMENU)ID_NOTICE, nullptr, nullptr);
        SendMessage(g_hNotice, WM_SETFONT, (WPARAM)g_hNoticeFont, TRUE);

        // 로그창 (y=96)
        g_hLog = CreateWindowW(L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            8, 96, 684, 280, hWnd, (HMENU)ID_LOG, nullptr, nullptr);
        SendMessage(g_hLog, WM_SETFONT, (WPARAM)g_hLogFont, TRUE);
        SendMessage(g_hLog, EM_SETLIMITTEXT, 200000, 0);

        // 입력창 (y=384)
        g_hInput = CreateWindowW(L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            8, 384, 470, 30, hWnd, (HMENU)ID_INPUT, nullptr, nullptr);
        SendMessage(g_hInput, WM_SETFONT, (WPARAM)g_hLogFont, TRUE);

        // 전송 버튼 (y=384)
        HWND hSend = CreateWindowW(L"BUTTON",
            L"\uC804\uC1A1", // 전송
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            486, 384, 60, 30, hWnd, (HMENU)ID_SEND, nullptr, nullptr);
        SendMessage(hSend, WM_SETFONT, (WPARAM)g_hBtnFont, TRUE);

        // SOS 버튼 (y=372, 크게)
        g_hSosBtn = CreateWindowW(L"BUTTON", L"SOS",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW,
            554, 372, 138, 50, hWnd, (HMENU)ID_SOS, nullptr, nullptr);
        SendMessage(g_hSosBtn, WM_SETFONT, (WPARAM)g_hSosFont, TRUE);

        AppendLogW(L"=== \uAC70\uC8FC\uC790 \uD654\uBA74 \uC2DC\uC791 ==="); // 거주자 화면 시작
        if (ConnectServer())
            AppendLogW(L"[\uC5F0\uACB0 \uC131\uACF5]"); // [연결 성공]
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

    case WM_UPDATENOTICE: {
        std::wstring* p = (std::wstring*)lParam;
        if (p) { SetWindowTextW(g_hNotice, p->c_str()); delete p; }
        break;
    }

                        // 알림판 배경색 (노란색)
    case WM_CTLCOLORSTATIC: {
        if ((HWND)lParam == g_hNotice) {
            HDC hdc = (HDC)wParam;
            SetBkColor(hdc, CLR_NOTICE_BG);
            SetTextColor(hdc, RGB(30, 30, 30));
            return (LRESULT)g_hYellowBrush;
        }
        if ((HWND)lParam == g_hClock) {
            HDC hdc = (HDC)wParam;
            SetBkColor(hdc, CLR_TITLEBAR);
            SetTextColor(hdc, RGB(210, 210, 200));
            return (LRESULT)g_hTitleBrush;
        }
        break;
    }

                          // SOS 버튼 직접 그리기 (빨간색)
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lParam;
        if (dis->CtlID == ID_SOS) {
            HBRUSH hbr = CreateSolidBrush(
                (dis->itemState & ODS_SELECTED) ? CLR_SOS_PRESS : CLR_SOS_NORMAL);
            FillRect(dis->hDC, &dis->rcItem, hbr);
            DeleteObject(hbr);
            SetTextColor(dis->hDC, RGB(255, 255, 255));
            SetBkMode(dis->hDC, TRANSPARENT);
            HFONT hOld = (HFONT)SelectObject(dis->hDC, g_hSosFont);
            DrawTextW(dis->hDC, L"SOS", -1, &dis->rcItem,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dis->hDC, hOld);
            return TRUE;
        }
        break;
    }

                    // 배경색
    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wParam;
        RECT rc; GetClientRect(hWnd, &rc);
        FillRect(hdc, &rc, g_hBgBrush);
        RECT tr = { 0,0,rc.right,22 };
        FillRect(hdc, &tr, g_hTitleBrush);
        return 1;
    }

    case WM_COMMAND: {
        switch (LOWORD(wParam)) {
        case ID_SEND: SendChat(); break;
        case ID_SOS:  SendSOS();  break;
        }
        break;
    }

    case WM_KEYDOWN: {
        if (wParam == VK_RETURN) {
            if (GetFocus() == g_hInput) SendChat();
            else {
                if (!g_sosHolding) {
                    g_sosHolding = true;
                    SetTimer(hWnd, TIMER_SOS, SOS_HOLD_MS, nullptr);
                }
            }
        }
        break;
    }
    case WM_KEYUP: {
        if (wParam == VK_RETURN) {
            KillTimer(hWnd, TIMER_SOS);
            g_sosHolding = false;
        }
        break;
    }

    case WM_TIMER: {
        if (wParam == TIMER_CLOCK) {
            SYSTEMTIME st; GetLocalTime(&st);
            wchar_t buf[64];
            swprintf(buf, 64, L"%04d-%02d-%02d  %02d:%02d:%02d",
                st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond);
            SetWindowTextW(g_hClock, buf);
        }
        if (wParam == TIMER_SOS && g_sosHolding) {
            KillTimer(hWnd, TIMER_SOS);
            g_sosHolding = false;
            SendSOS();
        }
        break;
    }

    case WM_DESTROY: {
        KillTimer(hWnd, TIMER_CLOCK);
        KillTimer(hWnd, TIMER_SOS);
        if (g_sock != INVALID_SOCKET) { closesocket(g_sock); WSACleanup(); }
        if (g_hTitleBrush)  DeleteObject(g_hTitleBrush);
        if (g_hBgBrush)     DeleteObject(g_hBgBrush);
        if (g_hYellowBrush) DeleteObject(g_hYellowBrush);
        if (g_hSosBrush)    DeleteObject(g_hSosBrush);
        if (g_hClockFont)   DeleteObject(g_hClockFont);
        if (g_hNoticeFont)  DeleteObject(g_hNoticeFont);
        if (g_hLogFont)     DeleteObject(g_hLogFont);
        if (g_hBtnFont)     DeleteObject(g_hBtnFont);
        if (g_hSosFont)     DeleteObject(g_hSosFont);
        PostQuitMessage(0);
        break;
    }
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

int main() {
    HINSTANCE hInst = GetModuleHandle(nullptr);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"SeniorUI";
    RegisterClassW(&wc);

    g_hWnd = CreateWindowW(L"SeniorUI",
        L"\uAC70\uC8FC\uC790 \uD654\uBA74 - Smart Home", // 거주자 화면
        WS_OVERLAPPEDWINDOW, 200, 100, 710, 460,
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