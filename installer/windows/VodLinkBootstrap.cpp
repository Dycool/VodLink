#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winhttp.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "VodLinkBootstrapBuild.h"

#ifndef VODLINK_RELEASE_TAG
#error "VodLinkBootstrapBuild.h must define VODLINK_RELEASE_TAG as a wide string literal"
#endif
#ifndef VODLINK_BUILD_COMMIT
#error "VodLinkBootstrapBuild.h must define VODLINK_BUILD_COMMIT as a wide string literal"
#endif
#ifndef VODLINK_SETUP_SHA256
#error "VodLinkBootstrapBuild.h must define VODLINK_SETUP_SHA256 as a narrow string literal"
#endif

namespace {
constexpr wchar_t kSetupUrl[] =
    L"https://github.com/Dycool/VodLink/releases/download/" VODLINK_RELEASE_TAG
    L"/VodLink-Windows-x64-Setup.exe";
constexpr wchar_t kBuildCommit[] = VODLINK_BUILD_COMMIT;
constexpr char kExpectedSetupHash[] = VODLINK_SETUP_SHA256;

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

bool launch(const std::filesystem::path &path)
{
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", path.c_str(), nullptr,
                                                    path.parent_path().c_str(), SW_SHOWNORMAL)) > 32;
}

bool download(const wchar_t *url, const std::filesystem::path &destination, std::wstring *error)
{
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url, 0, 0, &parts)) {
        *error = L"Could not parse the VodLink release URL.";
        return false;
    }

    HINTERNET session = WinHttpOpen(L"VodLink Installer/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        *error = L"Could not initialize the Windows download service.";
        return false;
    }
    WinHttpSetTimeouts(session, 15000, 15000, 30000, 30000);

    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring resource(parts.lpszUrlPath, parts.dwUrlPathLength);
    resource.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    HINTERNET connection = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    HINTERNET request = connection
        ? WinHttpOpenRequest(connection, L"GET", resource.c_str(), nullptr, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                             parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
        : nullptr;

    bool ok = request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        && WinHttpReceiveResponse(request, nullptr);
    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (ok) {
        ok = WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                                 WINHTTP_NO_HEADER_INDEX)
            && status == 200;
    }

    std::ofstream output;
    if (ok) {
        std::filesystem::create_directories(destination.parent_path());
        output.open(destination, std::ios::binary | std::ios::trunc);
        ok = output.good();
    }

    std::array<char, 64 * 1024> buffer{};
    while (ok) {
        DWORD received = 0;
        if (!WinHttpReadData(request, buffer.data(), static_cast<DWORD>(buffer.size()), &received)) {
            ok = false;
            break;
        }
        if (received == 0) {
            break;
        }
        output.write(buffer.data(), received);
        ok = output.good();
    }
    output.close();

    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    if (!ok) {
        std::error_code ignored;
        std::filesystem::remove(destination, ignored);
        *error = status == 404
            ? L"The VodLink release this launcher was built for does not contain the Windows installer."
            : L"The VodLink installer could not be downloaded.";
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
        + L"\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-";
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

int installInBackground()
{
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\VodLinkBackgroundInstaller");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    const auto cache = std::filesystem::temp_directory_path()
        / L"VodLink-Installer" / kBuildCommit;
    const auto setup = cache / L"VodLink-Windows-x64-Setup.exe";
    std::wstring error;
    bool ok = download(kSetupUrl, setup, &error);
    if (ok && !verifyDownload(setup)) {
        error = L"The downloaded installer does not match the VodLink build commit encoded in this launcher.";
        ok = false;
    }
    if (ok && !runSetup(setup)) {
        error = L"VodLink could not be installed.";
        ok = false;
    }
    if (ok && !launch(installedApp())) {
        error = L"VodLink was installed, but could not be opened.";
        ok = false;
    }

    std::error_code ignored;
    std::filesystem::remove(setup, ignored);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    if (!ok) {
        MessageBoxW(nullptr, error.c_str(), L"VodLink", MB_OK | MB_ICONERROR);
        return 1;
    }
    return 0;
}

bool spawnBackgroundWorker()
{
    std::array<wchar_t, 32768> executable{};
    if (!GetModuleFileNameW(nullptr, executable.data(),
                            static_cast<DWORD>(executable.size()))) return false;
    std::wstring command = L"\"" + std::wstring(executable.data()) + L"\" --install-background";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const bool started = CreateProcessW(executable.data(), mutableCommand.data(), nullptr, nullptr,
                                        FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr,
                                        &startup, &process);
    if (started) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    return started;
}
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int)
{
    if (std::wstring(commandLine).find(L"--install-background") != std::wstring::npos) {
        return installInBackground();
    }
    const auto app = installedApp();
    if (std::filesystem::is_regular_file(app)) {
        return launch(app) ? 0 : 1;
    }
    if (!spawnBackgroundWorker()) {
        MessageBoxW(nullptr, L"The VodLink background installer could not be started.",
                    L"VodLink", MB_OK | MB_ICONERROR);
        return 1;
    }
    return 0;
}
