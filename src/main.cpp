#include "probe.hpp"
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>

using namespace pingledger;
namespace fs = std::filesystem;

namespace {
constexpr UINT TrayMessage = WM_APP + 1;
constexpr int StartId = 101, StopId = 102, FolderId = 103, TrayId = 104;
constexpr COLORREF Ink = RGB(25, 39, 58), Blue = RGB(37, 99, 235), Muted = RGB(95, 111, 131);
HINSTANCE instance;
HWND window, targetsEdit, intervalEdit, latencyEdit, failuresEdit, list, startButton, stopButton, infoLabel;
HFONT font, titleFont, smallFont;
HBRUSH background;
NOTIFYICONDATAW tray{};
std::mutex stateMutex, waitMutex;
std::condition_variable wake;
std::atomic<bool> running{false};
std::thread worker;
fs::path dataDirectory, logFile, iniFile;
unsigned thresholdMs = 150, failureLimit = 3;
bool logError = false;
int selected = 0;
struct Target { std::string host; Statistics stats; Health previous = Health::waiting; };
struct Notice { std::wstring title, body; };
std::vector<Target> targets;
std::vector<Notice> notices;

std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), n);
    return result;
}
std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), n, nullptr, nullptr);
    return result;
}
std::wstring textOf(HWND control) {
    int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(control, text.data(), length + 1);
    text.resize(length);
    return text;
}
std::wstring decimal(double value) {
    std::wostringstream out;
    out << std::fixed << std::setprecision(1) << value;
    return out.str();
}
std::wstring healthText(Health value) {
    switch (value) {
    case Health::healthy: return L"Доступен";
    case Health::slow: return L"Нестабильно";
    case Health::down: return L"Нет ответа";
    default: return L"Ожидание";
    }
}
void balloon(const Notice& notice) {
    tray.uFlags = NIF_INFO;
    wcsncpy_s(tray.szInfoTitle, notice.title.c_str(), _TRUNCATE);
    wcsncpy_s(tray.szInfo, notice.body.c_str(), _TRUNCATE);
    tray.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &tray);
}
void stopMonitoring() {
    running = false;
    wake.notify_all();
    if (worker.joinable()) worker.join();
    EnableWindow(startButton, TRUE);
    EnableWindow(stopButton, FALSE);
    for (HWND control : {targetsEdit, intervalEdit, latencyEdit, failuresEdit}) EnableWindow(control, TRUE);
}
unsigned number(HWND control, unsigned min, unsigned max) {
    auto text = textOf(control);
    std::size_t consumed = 0;
    auto value = std::stoul(text, &consumed);
    if (consumed != text.size() || value < min || value > max) throw std::invalid_argument("range");
    return static_cast<unsigned>(value);
}
void startMonitoring() {
    try {
        auto hosts = parseTargets(utf8(textOf(targetsEdit)));
        auto interval = number(intervalEdit, 1, 3600);
        thresholdMs = number(latencyEdit, 1, 10000);
        failureLimit = number(failuresEdit, 1, 20);
        stopMonitoring();
        selected = 0;
        {
            std::lock_guard lock(stateMutex);
            targets.clear(); notices.clear(); logError = false;
            for (const auto& host : hosts) targets.push_back({host, {}, Health::waiting});
        }
        ListView_DeleteAllItems(list);
        for (std::size_t i = 0; i < hosts.size(); ++i) {
            auto value = wide(hosts[i]);
            LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = static_cast<int>(i); item.pszText = value.data();
            ListView_InsertItem(list, &item);
        }
        ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        std::wstring flattened;
        for (const auto& host : hosts) { if (!flattened.empty()) flattened += L","; flattened += wide(host); }
        WritePrivateProfileStringW(L"monitor", L"targets", flattened.c_str(), iniFile.c_str());
        WritePrivateProfileStringW(L"monitor", L"interval", std::to_wstring(interval).c_str(), iniFile.c_str());
        WritePrivateProfileStringW(L"monitor", L"threshold", std::to_wstring(thresholdMs).c_str(), iniFile.c_str());
        WritePrivateProfileStringW(L"monitor", L"failures", std::to_wstring(failureLimit).c_str(), iniFile.c_str());
        running = true;
        EnableWindow(startButton, FALSE); EnableWindow(stopButton, TRUE);
        for (HWND control : {targetsEdit, intervalEdit, latencyEdit, failuresEdit}) EnableWindow(control, FALSE);
        worker = std::thread([hosts, interval] {
            try {
                std::ofstream log(logFile, std::ios::app | std::ios::binary);
                if (log && log.tellp() == 0) log << "timestamp,target,status,latency_ms,detail\n";
                while (running) {
                    const auto cycle = std::chrono::steady_clock::now();
                    std::vector<std::future<Sample>> pending;
                    for (const auto& host : hosts) {
                        pending.push_back(std::async(std::launch::async, [host] { IcmpProbe probe; return probe.run(host, 1000); }));
                    }
                    for (std::size_t i = 0; i < hosts.size(); ++i) {
                        Sample sample = pending[i].get();
                        log << csvRow(hosts[i], sample);
                        log.flush();
                        std::lock_guard lock(stateMutex);
                        logError = !log.good();
                        auto& target = targets[i];
                        target.stats.add(sample);
                        auto current = health(target.stats, thresholdMs, failureLimit);
                        if (current != target.previous) {
                            if (current == Health::down || (current == Health::slow && sample.success))
                                notices.push_back({L"PingLedger: " + healthText(current), wide(target.host) + L". " +
                                    (sample.success ? L"Задержка " + std::to_wstring(sample.milliseconds) + L" мс" : L"Несколько проверок подряд без ответа")});
                            if (current == Health::healthy && target.previous != Health::waiting)
                                notices.push_back({L"PingLedger: связь восстановлена", wide(target.host) + L" снова отвечает"});
                        }
                        target.previous = current;
                    }
                    std::unique_lock lock(waitMutex);
                    wake.wait_until(lock, cycle + std::chrono::seconds(interval), [] { return !running; });
                }
            } catch (const std::exception& error) {
                std::lock_guard lock(stateMutex);
                notices.push_back({L"Ошибка мониторинга", wide(error.what())});
                running = false;
            }
        });
    } catch (const std::exception&) {
        MessageBoxW(window, L"Укажи от 1 до 16 IPv4-адресов или доменов без https:// и порта.\nИнтервал: 1–3600 с. Порог: 1–10000 мс. Ошибок подряд: 1–20.", L"Проверь настройки", MB_OK | MB_ICONWARNING);
    }
}
void setCell(int row, int column, const std::wstring& value) {
    ListView_SetItemText(list, row, column, const_cast<wchar_t*>(value.c_str()));
}
void refresh() {
    std::vector<Target> snapshot;
    std::vector<Notice> pending;
    bool error;
    {
        std::lock_guard lock(stateMutex);
        snapshot = targets; pending.swap(notices); error = logError;
    }
    for (std::size_t i = 0; i < snapshot.size(); ++i) {
        const auto& stats = snapshot[i].stats;
        int row = static_cast<int>(i);
        setCell(row, 1, healthText(health(stats, thresholdMs, failureLimit)));
        setCell(row, 2, stats.history.empty() || !stats.history.back().success ? L"—" : std::to_wstring(stats.history.back().milliseconds));
        setCell(row, 3, stats.received ? decimal(stats.average()) : L"—");
        setCell(row, 4, stats.sent ? decimal(stats.loss()) + L" %" : L"—");
        setCell(row, 5, stats.received ? std::to_wstring(stats.minimum) + L" / " + std::to_wstring(stats.maximum) : L"—");
        setCell(row, 6, decimal(stats.jitter()));
        setCell(row, 7, std::to_wstring(stats.sent));
    }
    if (!pending.empty()) balloon(pending.back());
    if (!running && worker.joinable()) stopMonitoring();
    std::wstring status = running ? L"Мониторинг активен. " : L"Мониторинг остановлен. ";
    status += error ? L"Не удалось записать CSV. Проверь доступ к папке данных." : L"История: " + logFile.wstring();
    SetWindowTextW(infoLabel, status.c_str());
    RECT rect{}; GetClientRect(window, &rect); rect.top = 386; rect.bottom -= 46;
    InvalidateRect(window, &rect, FALSE);
}
HWND control(const wchar_t* type, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id = 0) {
    auto handle = CreateWindowExW(type == std::wstring(L"EDIT") ? WS_EX_CLIENTEDGE : 0,
        type, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, window,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return handle;
}
void drawText(HDC dc, const std::wstring& value, RECT rect, HFONT selectedFont, COLORREF color, UINT format = DT_LEFT | DT_SINGLELINE | DT_VCENTER) {
    auto old = SelectObject(dc, selectedFont);
    SetTextColor(dc, color); SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, value.c_str(), -1, &rect, format);
    SelectObject(dc, old);
}
void paint(HDC destination, RECT client) {
    HDC dc = CreateCompatibleDC(destination);
    HBITMAP bitmap = CreateCompatibleBitmap(destination, client.right, client.bottom);
    auto oldBitmap = SelectObject(dc, bitmap);
    FillRect(dc, &client, background);
    RECT header{0, 0, client.right, 86}; HBRUSH dark = CreateSolidBrush(Ink); FillRect(dc, &header, dark); DeleteObject(dark);
    drawText(dc, L"PingLedger", {24, 13, 400, 48}, titleFont, RGB(255, 255, 255));
    drawText(dc, L"Доступность сети, задержка и потери пакетов", {25, 49, client.right - 24, 75}, font, RGB(190, 207, 229));
    drawText(dc, L"АДРЕСА ДЛЯ ПРОВЕРКИ", {24, 95, 580, 117}, smallFont, Muted);
    drawText(dc, L"Через запятую или с новой строки. До 16 адресов.", {24, 187, 600, 210}, smallFont, Muted);
    drawText(dc, L"Интервал, с", {610, 96, 735, 118}, smallFont, Muted);
    drawText(dc, L"Порог, мс", {735, 96, 850, 118}, smallFont, Muted);
    drawText(dc, L"Ошибок подряд", {850, 96, 990, 118}, smallFont, Muted);
    std::vector<Target> snapshot;
    { std::lock_guard lock(stateMutex); snapshot = targets; }
    std::wstring graphTitle = L"ЗАДЕРЖКА · последние 120 проверок";
    if (selected >= 0 && selected < static_cast<int>(snapshot.size())) graphTitle += L" · " + wide(snapshot[selected].host);
    drawText(dc, graphTitle, {24, 391, client.right - 24, 418}, smallFont, Muted);
    RECT graph{60, 435, client.right - 28, client.bottom - 94};
    if (graph.bottom > graph.top && graph.right > graph.left) {
        HBRUSH white = CreateSolidBrush(RGB(255, 255, 255)); FillRect(dc, &graph, white); DeleteObject(white);
        unsigned scale = std::max(100u, thresholdMs + thresholdMs / 4);
        const Statistics* stats = nullptr;
        if (selected >= 0 && selected < static_cast<int>(snapshot.size())) {
            stats = &snapshot[selected].stats;
            for (const auto& sample : stats->history) if (sample.success) scale = std::max(scale, sample.milliseconds + 20);
        }
        auto grid = CreatePen(PS_SOLID, 1, RGB(226, 232, 240)); auto oldPen = SelectObject(dc, grid);
        for (int i = 0; i <= 4; ++i) {
            int y = graph.bottom - (graph.bottom - graph.top) * i / 4;
            MoveToEx(dc, graph.left, y, nullptr); LineTo(dc, graph.right, y);
            drawText(dc, std::to_wstring(scale * i / 4), {6, y - 9, 52, y + 10}, smallFont, Muted, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
        }
        SelectObject(dc, oldPen); DeleteObject(grid);
        auto thresholdPen = CreatePen(PS_DASH, 1, RGB(217, 119, 6)); SelectObject(dc, thresholdPen);
        int thresholdY = graph.bottom - static_cast<int>(static_cast<double>(thresholdMs) / scale * (graph.bottom - graph.top));
        MoveToEx(dc, graph.left, thresholdY, nullptr); LineTo(dc, graph.right, thresholdY);
        SelectObject(dc, oldPen); DeleteObject(thresholdPen);
        if (stats && !stats->history.empty()) {
            auto line = CreatePen(PS_SOLID, 2, Blue); SelectObject(dc, line);
            bool connected = false;
            for (std::size_t i = 0; i < stats->history.size(); ++i) {
                auto& sample = stats->history[i];
                int x = graph.left + static_cast<int>(i * static_cast<std::size_t>(graph.right - graph.left - 4) / 119) + 2;
                int y = graph.bottom - static_cast<int>(static_cast<double>(sample.milliseconds) / scale * (graph.bottom - graph.top)) - 1;
                if (sample.success) {
                    if (connected) LineTo(dc, x, y); else MoveToEx(dc, x, y, nullptr);
                    SetPixel(dc, x, y, Blue); connected = true;
                } else {
                    auto failed = CreatePen(PS_SOLID, 2, RGB(220, 38, 38)); auto previous = SelectObject(dc, failed);
                    MoveToEx(dc, x - 3, graph.bottom - 7, nullptr); LineTo(dc, x + 3, graph.bottom - 1);
                    MoveToEx(dc, x - 3, graph.bottom - 1, nullptr); LineTo(dc, x + 3, graph.bottom - 7);
                    SelectObject(dc, previous); DeleteObject(failed); connected = false;
                }
            }
            SelectObject(dc, oldPen); DeleteObject(line);
        } else {
            drawText(dc, L"Нажми «Начать», чтобы получить первые измерения", graph, font, Muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        drawText(dc, L"Синий: задержка   ·   Красный: нет ответа   ·   Пунктир: порог уведомления", {60, graph.bottom + 10, client.right - 24, graph.bottom + 35}, smallFont, Muted);
    }
    BitBlt(destination, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBitmap); DeleteObject(bitmap); DeleteDC(dc);
}
void layout() {
    RECT rect{}; GetClientRect(window, &rect);
    MoveWindow(list, 24, 222, rect.right - 48, 158, TRUE);
    MoveWindow(infoLabel, 24, rect.bottom - 42, rect.right - 48, 30, TRUE);
}
LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        window = hwnd;
        wchar_t saved[8192]{};
        GetPrivateProfileStringW(L"monitor", L"targets", L"127.0.0.1, 1.1.1.1, example.com", saved, 8192, iniFile.c_str());
        targetsEdit = control(L"EDIT", saved, ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, 24, 122, 560, 64);
        GetPrivateProfileStringW(L"monitor", L"interval", L"3", saved, 8192, iniFile.c_str());
        intervalEdit = control(L"EDIT", saved, ES_NUMBER | WS_TABSTOP, 610, 122, 96, 30);
        GetPrivateProfileStringW(L"monitor", L"threshold", L"150", saved, 8192, iniFile.c_str());
        latencyEdit = control(L"EDIT", saved, ES_NUMBER | WS_TABSTOP, 735, 122, 96, 30);
        GetPrivateProfileStringW(L"monitor", L"failures", L"3", saved, 8192, iniFile.c_str());
        failuresEdit = control(L"EDIT", saved, ES_NUMBER | WS_TABSTOP, 850, 122, 96, 30);
        startButton = control(L"BUTTON", L"Начать", BS_PUSHBUTTON | WS_TABSTOP, 610, 164, 100, 34, StartId);
        stopButton = control(L"BUTTON", L"Остановить", BS_PUSHBUTTON | WS_TABSTOP, 720, 164, 110, 34, StopId);
        control(L"BUTTON", L"История CSV", BS_PUSHBUTTON | WS_TABSTOP, 840, 164, 120, 34, FolderId);
        EnableWindow(stopButton, FALSE);
        list = control(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, 24, 222, 936, 158);
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        const wchar_t* columns[] = {L"Адрес", L"Состояние", L"Сейчас, мс", L"Среднее", L"Потери", L"Мин / макс", L"Джиттер", L"Проверок"};
        int widths[] = {205, 135, 100, 95, 90, 110, 90, 90};
        for (int i = 0; i < 8; ++i) {
            LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH; column.pszText = const_cast<wchar_t*>(columns[i]); column.cx = widths[i];
            ListView_InsertColumn(list, i, &column);
        }
        infoLabel = control(L"STATIC", L"Готов к работе. Измерения сохраняются локально.", SS_LEFT, 24, 640, 936, 30);
        tray.cbSize = sizeof(tray); tray.hWnd = hwnd; tray.uID = TrayId; tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        tray.uCallbackMessage = TrayMessage; tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(tray.szTip, L"PingLedger · мониторинг сети");
        Shell_NotifyIconW(NIM_ADD, &tray);
        SetTimer(hwnd, 1, 500, nullptr);
        layout(); return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
        limits->ptMinTrackSize = {1010, 650}; return 0;
    }
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) { ShowWindow(hwnd, SW_HIDE); return 0; }
        layout(); InvalidateRect(hwnd, nullptr, TRUE); return 0;
    case WM_COMMAND:
        if (LOWORD(wParam) == StartId) startMonitoring();
        if (LOWORD(wParam) == StopId) stopMonitoring();
        if (LOWORD(wParam) == FolderId) ShellExecuteW(hwnd, L"open", dataDirectory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return 0;
    case WM_NOTIFY:
        if (reinterpret_cast<NMHDR*>(lParam)->hwndFrom == list && reinterpret_cast<NMHDR*>(lParam)->code == LVN_ITEMCHANGED) {
            int row = ListView_GetNextItem(list, -1, LVNI_SELECTED);
            if (row >= 0) selected = row;
        }
        return 0;
    case WM_TIMER: refresh(); return 0;
    case TrayMessage:
        if (lParam == WM_LBUTTONDBLCLK) { ShowWindow(hwnd, SW_RESTORE); SetForegroundWindow(hwnd); }
        if (lParam == WM_RBUTTONUP) {
            auto menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, L"Открыть PingLedger");
            AppendMenuW(menu, MF_STRING, 2, L"Завершить работу");
            POINT point{}; GetCursorPos(&point); SetForegroundWindow(hwnd);
            auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
            if (command == 1) { ShowWindow(hwnd, SW_RESTORE); SetForegroundWindow(hwnd); }
            if (command == 2) SendMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
    case WM_CTLCOLORSTATIC:
        SetTextColor(reinterpret_cast<HDC>(wParam), Muted);
        SetBkColor(reinterpret_cast<HDC>(wParam), RGB(247, 249, 252));
        return reinterpret_cast<LRESULT>(background);
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paintStruct{}; auto dc = BeginPaint(hwnd, &paintStruct);
        RECT rect{}; GetClientRect(hwnd, &rect); paint(dc, rect); EndPaint(hwnd, &paintStruct); return 0;
    }
    case WM_CLOSE:
        KillTimer(hwnd, 1); stopMonitoring(); Shell_NotifyIconW(NIM_DELETE, &tray); DestroyWindow(hwnd); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool screenshot(const fs::path& path) {
    RECT rect{}; GetClientRect(window, &rect);
    auto screen = GetDC(window); auto dc = CreateCompatibleDC(screen);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = rect.right; info.bmiHeader.biHeight = -rect.bottom;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    auto bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap) { DeleteDC(dc); ReleaseDC(window, screen); return false; }
    auto old = SelectObject(dc, bitmap);
    paint(dc, rect);
    for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        int saved = SaveDC(dc);
        RECT childRect{}; GetWindowRect(child, &childRect); MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&childRect), 2);
        SetViewportOrgEx(dc, childRect.left, childRect.top, nullptr);
        SendMessageW(child, WM_PRINT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_NONCLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
        RestoreDC(dc, saved);
    }
    SetViewportOrgEx(dc, 0, 0, nullptr);
    GdiFlush();
    DWORD size = static_cast<DWORD>(rect.right * rect.bottom * 4);
    BITMAPFILEHEADER header{}; header.bfType = 0x4D42; header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER); header.bfSize = header.bfOffBits + size;
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<char*>(&header), sizeof(header));
    output.write(reinterpret_cast<char*>(&info.bmiHeader), sizeof(BITMAPINFOHEADER));
    output.write(reinterpret_cast<char*>(pixels), size);
    bool ok = output.good();
    SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(window, screen);
    return ok;
}
} // namespace

int WINAPI wWinMain(HINSTANCE appInstance, HINSTANCE, PWSTR arguments, int show) {
    try {
        instance = appInstance;
        SetProcessDPIAware();
        Winsock winsock;
        PWSTR local = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) throw std::runtime_error("LocalAppData unavailable");
        dataDirectory = fs::path(local) / L"PingLedger"; CoTaskMemFree(local);
        bool smoke = std::wstring(arguments).find(L"--smoke") != std::wstring::npos;
        if (smoke) dataDirectory /= L"smoke";
        fs::create_directories(dataDirectory);
        iniFile = dataDirectory / L"settings.ini";
        auto date = timestamp(std::chrono::system_clock::now()).substr(0, 10);
        logFile = dataDirectory / ("measurements-" + date + ".csv");
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES}; InitCommonControlsEx(&controls);
        font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        titleFont = CreateFontW(-28, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        smallFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        background = CreateSolidBrush(RGB(247, 249, 252));
        WNDCLASSW cls{}; cls.lpfnWndProc = procedure; cls.hInstance = instance; cls.lpszClassName = L"PingLedgerWindow";
        cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        if (!RegisterClassW(&cls)) throw std::runtime_error("Window registration failed");
        window = CreateWindowExW(0, cls.lpszClassName, L"PingLedger | Мониторинг сети", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            CW_USEDEFAULT, CW_USEDEFAULT, 1030, 740, nullptr, nullptr, instance, nullptr);
        if (!window) throw std::runtime_error("Window creation failed");
        int result = 0;
        if (smoke) {
            SetWindowPos(window, nullptr, -20000, -20000, 0, 0, SWP_NOACTIVATE | SWP_NOSIZE | SWP_NOZORDER);
            ShowWindow(window, SW_SHOWNOACTIVATE);
            UpdateWindow(window);
            SetWindowTextW(targetsEdit, L"127.0.0.1"); SetWindowTextW(intervalEdit, L"1");
            startMonitoring();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
            while (std::chrono::steady_clock::now() < deadline) {
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
                Sleep(25);
            }
            stopMonitoring(); refresh();
            { std::lock_guard lock(stateMutex); if (targets.empty() || targets[0].stats.received < 2 || logError) result = 2; }
            if (!screenshot(dataDirectory / L"smoke.bmp")) result = 3;
            SendMessageW(window, WM_CLOSE, 0, 0);
        } else {
            ShowWindow(window, show); UpdateWindow(window);
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
            }
        }
        DeleteObject(font); DeleteObject(titleFont); DeleteObject(smallFont); DeleteObject(background);
        return result;
    } catch (const std::exception& error) {
        MessageBoxW(nullptr, wide(error.what()).c_str(), L"PingLedger", MB_OK | MB_ICONERROR);
        return 1;
    }
}
