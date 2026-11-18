#pragma once
// Updates from GitHub. Each release of the tekken-studio repo has the setup .exe and a .sha256 file with its SHA-256. The tool
// asks GitHub for the latest release when it opens, and if it's newer than this build, offers to download it, checks the
// SHA-256 and runs it. While the repo is private GitHub answers 404, and the tool simply offers nothing.
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <winhttp.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "version.lib")

namespace updater {

inline constexpr const wchar_t* ReleasesRepo = L"mussaa3d/tekken-studio";

struct Release {
    bool Found = false;
    std::string Version;     // "2.2"
    std::string InstallerUrl, ChecksumUrl, InstallerName;
};

// GET over HTTPS, following redirects (GitHub sends downloads on to its file servers). Returns the HTTP status, or 0.
inline int HttpGet(const std::wstring& url, std::string& body, size_t limit = 64u << 20)
{
    body.clear();
    URL_COMPONENTS parts { sizeof(parts) };
    wchar_t host[256] = L"", path[2048] = L"";
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
        return 0;
    int status = 0;
    HINTERNET session = WinHttpOpen(L"tekken8mvm-updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
        return 0;
    WinHttpSetTimeouts(session, 15000, 15000, 30000, 60000);
    HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (request && WinHttpSendRequest(request, L"Accept: application/vnd.github+json\r\n", static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        && WinHttpReceiveResponse(request, nullptr)) {
        DWORD code = 0, size = sizeof(code);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
        status = static_cast<int>(code);
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
            if (body.size() + available > limit) {
                status = 0;
                break;
            }
            const size_t at = body.size();
            body.resize(at + available);
            DWORD read = 0;
            if (!WinHttpReadData(request, body.data() + at, available, &read)) {
                status = 0;
                break;
            }
            body.resize(at + read);
            if (read == 0)
                break;
        }
    }
    if (request)
        WinHttpCloseHandle(request);
    if (connection)
        WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return status;
}

// The string value of "key" at or after `from`, or "".
inline std::string JsonValue(const std::string& json, const std::string& key, size_t from = 0, size_t* found = nullptr)
{
    const size_t at = json.find("\"" + key + "\"", from);
    if (at == std::string::npos)
        return {};
    size_t quote = json.find(':', at);
    quote = quote == std::string::npos ? quote : json.find_first_not_of(" \t\r\n", quote + 1);
    if (quote == std::string::npos || json[quote] != '"')
        return {};
    const size_t end = json.find('"', quote + 1);
    if (found)
        *found = end;
    return end == std::string::npos ? std::string() : json.substr(quote + 1, end - quote - 1);
}

// Reads GitHub's /releases/latest reply: the version from the tag, and the setup .exe and .sha256 assets.
inline Release ReadLatest(const std::string& json)
{
    Release release;
    std::string tag = JsonValue(json, "tag_name");
    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V'))
        tag.erase(0, 1);
    release.Version = tag;
    size_t at = json.find("\"assets\"");
    while (at != std::string::npos) {
        size_t next = std::string::npos;
        const std::string url = JsonValue(json, "browser_download_url", at, &next);
        if (url.empty())
            break;
        const std::string name = url.substr(url.rfind('/') + 1);
        if (name.size() > 10 && name.ends_with("-setup.exe")) {
            release.InstallerUrl = url;
            release.InstallerName = name;
        } else if (name.ends_with(".sha256")) {
            release.ChecksumUrl = url;
        }
        at = next;
    }
    release.Found = !release.Version.empty() && !release.InstallerUrl.empty() && !release.ChecksumUrl.empty();
    return release;
}

// Compares dotted versions number by number: "2.10" is newer than "2.9".
inline bool Newer(const std::string& candidate, const std::string& current)
{
    size_t a = 0, b = 0;
    while (a < candidate.size() || b < current.size()) {
        const long x = a < candidate.size() ? strtol(candidate.c_str() + a, nullptr, 10) : 0;
        const long y = b < current.size() ? strtol(current.c_str() + b, nullptr, 10) : 0;
        if (x != y)
            return x > y;
        a = candidate.find('.', a);
        b = current.find('.', b);
        a = a == std::string::npos ? candidate.size() : a + 1;
        b = b == std::string::npos ? current.size() : b + 1;
    }
    return false;
}

// This build's version, from the exe's own ProductVersion ("0.1").
inline std::string CurrentVersion()
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(exe, &ignored);
    std::vector<char> data(size);
    wchar_t* value = nullptr;
    UINT length = 0;
    if (size && GetFileVersionInfoW(exe, 0, size, data.data())
        && VerQueryValueW(data.data(), L"\\StringFileInfo\\040904b0\\ProductVersion", reinterpret_cast<void**>(&value), &length) && value) {
        std::string out;
        for (const wchar_t* c = value; *c; ++c)
            out += static_cast<char>(*c);
        return out;
    }
    return "0";
}

inline Release Latest()
{
    std::string body;
    if (HttpGet(std::wstring(L"https://api.github.com/repos/") + ReleasesRepo + L"/releases/latest", body, 1u << 20) != 200)
        return {};
    return ReadLatest(body);
}

inline std::string Sha256Hex(const std::string& data)
{
    unsigned char hash[32];
    if (!BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                                   static_cast<ULONG>(data.size()), hash, sizeof(hash))))
        return {};
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (const unsigned char b : hash) {
        out += hex[b >> 4];
        out += hex[b & 15];
    }
    return out;
}

// Downloads the installer, checks it against the published SHA-256 and starts it. Returns "" when it started, else what went wrong.
inline std::string DownloadAndRun(const Release& release)
{
    std::string checksum, installer;
    if (HttpGet(std::wstring(release.ChecksumUrl.begin(), release.ChecksumUrl.end()), checksum, 4096) != 200)
        return "Couldn't download the update's checksum. Try again later.";
    std::string expected;
    for (const char c : checksum) {
        if (isxdigit(static_cast<unsigned char>(c)))
            expected += static_cast<char>(tolower(c));
        else if (!expected.empty())
            break;
    }
    if (expected.size() != 64)
        return "The update's checksum file isn't right. Try again later.";
    if (HttpGet(std::wstring(release.InstallerUrl.begin(), release.InstallerUrl.end()), installer) != 200)
        return "Couldn't download the update. Try again later.";
    if (Sha256Hex(installer) != expected)
        return "The downloaded update doesn't match its checksum, so it wasn't run.";
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    const std::filesystem::path path = std::filesystem::path(temp) / std::wstring(release.InstallerName.begin(), release.InstallerName.end());
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(installer.data(), static_cast<std::streamsize>(installer.size()));
        if (!file)
            return "Couldn't save the update.";
    }
    // Silent: only a progress bar, into the same folder with the same choices as before, and the installer starts the tool
    // again when it's done. The tool closes as soon as this starts, so nothing is in use.
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", path.c_str(), L"/SILENT /SUPPRESSMSGBOXES /NORESTART /NOCANCEL /CLOSEAPPLICATIONS",
                                                nullptr, SW_SHOWNORMAL)) <= 32)
        return "Couldn't start the update.";
    return {};
}

} // namespace updater
