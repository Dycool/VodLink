#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "VodLinkBootstrapBuild.h"

#pragma comment(lib, "gdi32.lib")

#ifndef VODLINK_BUILD_COMMIT
#error "VodLinkBootstrapBuild.h must define VODLINK_BUILD_COMMIT as a wide string literal"
#endif
#ifndef VODLINK_SETUP_SHA256
#error "VodLinkBootstrapBuild.h must define VODLINK_SETUP_SHA256 as a narrow string literal"
#endif

namespace {
constexpr wchar_t kBuildCommit[] = VODLINK_BUILD_COMMIT;
constexpr char kExpectedSetupHash[] = VODLINK_SETUP_SHA256;
constexpr int kEmbeddedSetupResource = 2;
constexpr wchar_t kWindowClass[] = L"VodLinkBootstrapWindow";
constexpr UINT kInstallFinished = WM_APP + 1;
constexpr UINT kInstallStatus = WM_APP + 2;
bool gLaunchMinimized = false;

struct InstallResult {
    bool ok = false;
    std::wstring error;
};

struct WindowState {
    int animationFrame = 0;
    std::wstring status = L"Preparing installer";
    bool installing = true;
};

std::filesystem::path localAppData()
{
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &raw))) {
        return {};
    }
    std::filesystem::path path(raw);
    CoTaskMemFree(raw);
    return path;
}

std::filesystem::path installedApp()
{
    return localAppData() / L"VodLink" / L"app" / L"VodLink.exe";
}

std::filesystem::path installedCommitMarker()
{
    return installedApp().parent_path() / L".vodlink-build-commit";
}

bool installedCommitMatches()
{
    std::wifstream input(installedCommitMarker());
    std::wstring commit;
    if (!std::getline(input, commit)) {
        return false;
    }
    return commit == kBuildCommit;
}

bool writeInstalledCommit()
{
    std::wofstream output(installedCommitMarker(), std::ios::trunc);
    output << kBuildCommit;
    return output.good();
}

bool launch(const std::filesystem::path &path, const wchar_t *arguments = nullptr)
{
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", path.c_str(), arguments,
                                                    path.parent_path().c_str(), SW_SHOWNORMAL)) > 32;
}

bool extractEmbeddedSetup(const std::filesystem::path &destination, std::wstring *error)
{
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(kEmbeddedSetupResource), RT_RCDATA);
    if (!resource) {
        *error = L"This VodLink installer does not contain its setup payload.";
        return false;
    }
    HGLOBAL loaded = LoadResource(module, resource);
    const void *data = loaded ? LockResource(loaded) : nullptr;
    const DWORD size = SizeofResource(module, resource);
    if (!data || size == 0) {
        *error = L"The embedded VodLink setup payload is invalid.";
        return false;
    }
    try {
        std::filesystem::create_directories(destination.parent_path());
    } catch (...) {
        *error = L"Could not create the temporary VodLink installer directory.";
        return false;
    }
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    output.write(static_cast<const char *>(data), size);
    const bool ok = output.good();
    output.close();
    if (!ok) {
        std::error_code ignored;
        std::filesystem::remove(destination, ignored);
        *error = L"The embedded VodLink setup could not be extracted.";
    }
    return ok;
}

std::string sha256(const std::filesystem::path &path)
{
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectSize = 0;
    DWORD resultSize = 0;
    std::vector<UCHAR> object;
    std::array<UCHAR, 32> digest{};
    std::string result;

    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0
        || BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                             reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize),
                             &resultSize, 0) < 0) {
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }
    object.resize(objectSize);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }

    std::ifstream input(path, std::ios::binary);
    std::array<char, 1024 * 1024> buffer{};
    while (input.good()) {
        input.read(buffer.data(), buffer.size());
        const auto count = input.gcount();
        if (count > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()),
                                       static_cast<ULONG>(count), 0) < 0) {
            input.setstate(std::ios::badbit);
        }
    }
    if (!input.eof()
        || BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
        BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }

    constexpr char hex[] = "0123456789abcdef";
    result.reserve(64);
    for (const UCHAR byte : digest) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 0x0f]);
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return result;
}

bool verifyDownload(const std::filesystem::path &setup)
{
    constexpr std::size_t kSha256HexLength = 64;
    return std::char_traits<char>::length(kExpectedSetupHash) == kSha256HexLength
        && sha256(setup) == kExpectedSetupHash;
}

bool runSetup(const std::filesystem::path &setup)
{
    std::wstring command = L"\"" + setup.wstring()
        + L"\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP- /FORCECLOSEAPPLICATIONS /VODLINKUPDATE";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    if (!CreateProcessW(setup.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, setup.parent_path().c_str(), &startup, &process)) {
        return false;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exitCode == 0;
}

InstallResult install(HWND window)
{
    const auto cache = std::filesystem::temp_directory_path()
        / L"VodLink-Installer" / kBuildCommit;
    const auto setup = cache / L"VodLink-Windows-x64-Setup.exe";
    std::wstring error;
    bool ok = extractEmbeddedSetup(setup, &error);
    if (ok && window) PostMessageW(window, kInstallStatus, 0, reinterpret_cast<LPARAM>(L"Verifying installer"));
    if (ok && !verifyDownload(setup)) {
        error = L"The embedded installer does not match the VodLink build commit encoded in this launcher.";
        ok = false;
    }
    if (ok && window) PostMessageW(window, kInstallStatus, 0, reinterpret_cast<LPARAM>(L"Installing VodLink"));
    if (ok && !runSetup(setup)) {
        error = L"VodLink could not be installed.";
        ok = false;
    }
    if (ok && !writeInstalledCommit()) {
        error = L"VodLink was installed, but its build marker could not be written.";
        ok = false;
    }
    if (ok && window) PostMessageW(window, kInstallStatus, 0, reinterpret_cast<LPARAM>(L"Opening VodLink"));

    std::error_code ignored;
    std::filesystem::remove(setup, ignored);
    return {ok, std::move(error)};
}

DWORD WINAPI installWorker(void *context)
{
    auto *result = new InstallResult(install(static_cast<HWND>(context)));
    if (result->ok
        && !launch(installedApp(), gLaunchMinimized ? L"--minimized" : nullptr)) {
        result->ok = false;
        result->error = L"VodLink was installed, but could not be opened.";
    }
    PostMessageW(static_cast<HWND>(context), kInstallFinished, 0,
                 reinterpret_cast<LPARAM>(result));
    return 0;
}

void drawCenteredText(HDC dc, const RECT &area, const wchar_t *text, HFONT font, COLORREF color)
{
    HGDIOBJ previousFont = SelectObject(dc, font);
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    RECT target = area;
    DrawTextW(dc, text, -1, &target, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, previousFont);
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto *state = reinterpret_cast<WindowState *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_CREATE:
        state = new WindowState;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        SetTimer(window, 1, 180, nullptr);
        return 0;
    case WM_TIMER:
        if (state) {
            state->animationFrame = (state->animationFrame + 1) % 8;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        HBRUSH background = CreateSolidBrush(RGB(8, 13, 22));
        FillRect(dc, &client, background);
        DeleteObject(background);

        HPEN border = CreatePen(PS_SOLID, 1, RGB(64, 48, 104));
        HGDIOBJ previousBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        HGDIOBJ previousPen = SelectObject(dc, border);
        RoundRect(dc, 1, 1, client.right - 1, client.bottom - 1, 20, 20);
        SelectObject(dc, previousPen);
        SelectObject(dc, previousBrush);
        DeleteObject(border);

        HICON icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1),
                                                   IMAGE_ICON, 72, 72, LR_DEFAULTCOLOR));
        if (icon) {
            DrawIconEx(dc, (client.right - 72) / 2, 24, icon, 72, 72, 0, nullptr, DI_NORMAL);
            DestroyIcon(icon);
        }

        HFONT titleFont = CreateFontW(32, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        RECT title{0, 99, client.right, 140};
        drawCenteredText(dc, title, L"VodLink", titleFont, RGB(244, 247, 251));
        DeleteObject(titleFont);

        HFONT statusFont = CreateFontW(17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        RECT statusRect{0, 151, client.right, 180};
        drawCenteredText(dc, statusRect, state ? state->status.c_str() : L"Installing VodLink",
                         statusFont, RGB(183, 191, 208));
        DeleteObject(statusFont);

        if (state) {
            for (int i = 0; i < 8; ++i) {
                const double angle = (i * 3.141592653589793 * 2.0) / 8.0;
                const int x = client.right / 2 + static_cast<int>(24 * cos(angle));
                const int y = 211 + static_cast<int>(9 * sin(angle));
                const int distance = (i - state->animationFrame + 8) % 8;
                const int brightness = 75 + (7 - distance) * 20;
                const int red = brightness < 143 ? brightness : 143;
                const int green = brightness < 102 ? brightness : 102;
                const int blue = brightness + 70 < 255 ? brightness + 70 : 255;
                HBRUSH dot = CreateSolidBrush(RGB(red, green, blue));
                RECT circle{x - 3, y - 3, x + 4, y + 4};
                HBRUSH previous = static_cast<HBRUSH>(SelectObject(dc, dot));
                Ellipse(dc, circle.left, circle.top, circle.right, circle.bottom);
                SelectObject(dc, previous);
                DeleteObject(dot);
            }
        }
        EndPaint(window, &paint);
        return 0;
    }
    case kInstallStatus:
        if (state) {
            state->status = reinterpret_cast<const wchar_t *>(lParam);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case kInstallFinished: {
        auto *result = reinterpret_cast<InstallResult *>(lParam);
        if (state) state->installing = false;
        if (!result->ok) {
            MessageBoxW(window, result->error.c_str(), L"VodLink", MB_OK | MB_ICONERROR);
        }
        delete result;
        DestroyWindow(window);
        return 0;
    }
    case WM_CLOSE:
        if (state && state->installing) return 0;
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, 1);
        delete state;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int)
{
    const std::wstring arguments(commandLine ? commandLine : L"");
    const bool backgroundUpdate = arguments.find(L"--update-background") != std::wstring::npos;
    const bool launchMinimized = arguments.find(L"--launch-minimized") != std::wstring::npos;
    gLaunchMinimized = launchMinimized;
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\VodLinkInstaller");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kWindowClass, nullptr)) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }
    const auto app = installedApp();
    if (std::filesystem::is_regular_file(app) && installedCommitMatches()) {
        const int result = launch(app, launchMinimized ? L"--minimized" : nullptr) ? 0 : 1;
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return result;
    }

    if (backgroundUpdate) {
        const InstallResult installResult = install(nullptr);
        const bool launched = installResult.ok
            && launch(installedApp(), launchMinimized ? L"--minimized" : nullptr);
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return launched ? 0 : 1;
    }

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass)) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return 1;
    }
    const int width = 400;
    const int height = 270;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    HWND window = CreateWindowExW(WS_EX_APPWINDOW, kWindowClass, L"Installing VodLink",
                                  WS_POPUP, x, y, width, height, nullptr, nullptr, instance, nullptr);
    if (!window) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return 1;
    }
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    HANDLE worker = CreateThread(nullptr, 0, installWorker, window, 0, nullptr);
    if (!worker) {
        MessageBoxW(window, L"The VodLink installer could not be started.", L"VodLink",
                    MB_OK | MB_ICONERROR);
        DestroyWindow(window);
    } else {
        CloseHandle(worker);
    }
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return static_cast<int>(message.wParam);
}
