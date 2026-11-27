// iHeart Radio - local MP3 bridge (see Bridge.hpp).
#include "Bridge.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <winhttp.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace
{
constexpr unsigned short kPort = 8925;
constexpr int kBitrate = 192;

fs::path g_log;
fs::path g_nowFile;
std::wstring g_ffmpeg;
std::mutex g_logMutex;
std::mutex g_stateMutex;
std::map<std::string, int> g_active;              // station id -> listeners
std::map<std::string, std::string> g_srcs;        // station id -> stream URL
std::map<std::string, std::string> g_lastTrack;   // station id -> last logged text (poller thread only)
std::atomic<bool> g_stop{false};
SOCKET g_listen = INVALID_SOCKET;
HANDLE g_job = nullptr;

// ------------------------------------------------------------------ text helpers
std::string Utf8(const std::wstring& aText)
{
    if (aText.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, aText.data(), static_cast<int>(aText.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, aText.data(), static_cast<int>(aText.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Wide(const std::string& aText)
{
    if (aText.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, aText.data(), static_cast<int>(aText.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, aText.data(), static_cast<int>(aText.size()), out.data(), n);
    return out;
}

void Log(const std::string& aText)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[32];
    sprintf_s(stamp, "[%04u-%02u-%02u %02u:%02u:%02u] ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::lock_guard lock(g_logMutex);
    std::ofstream f(g_log, std::ios::app | std::ios::binary);
    f << stamp << aText << "\r\n";
}

std::string Trim(const std::string& s)
{
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

void ReplaceAll(std::string& s, const std::string& from, const std::string& to)
{
    for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size()))
        s.replace(p, from.size(), to);
}

// typographic quotes / dashes -> plain ones (safer for the game's fonts), no tabs / line breaks
std::string Clean(std::string s)
{
    ReplaceAll(s, "\xE2\x80\x98", "'");
    ReplaceAll(s, "\xE2\x80\x99", "'");
    ReplaceAll(s, "\xE2\x80\x9C", "\"");
    ReplaceAll(s, "\xE2\x80\x9D", "\"");
    ReplaceAll(s, "\xE2\x80\x93", "-");
    ReplaceAll(s, "\xE2\x80\x94", "-");
    ReplaceAll(s, "\xE2\x80\xA6", "...");
    for (auto& c : s)
        if (c == '\t' || c == '\r' || c == '\n')
            c = ' ';
    return Trim(s);
}

// ------------------------------------------------------------------ minimal JSON (iHeart API answers)
struct Json
{
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    std::string str;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> fields;

    const Json* Get(const char* aKey) const
    {
        if (type != Type::Object)
            return nullptr;
        for (const auto& [k, v] : fields)
            if (k == aKey)
                return &v;
        return nullptr;
    }
    std::string Str(const char* aKey) const
    {
        const Json* v = Get(aKey);
        return v && v->type == Type::String ? v->str : std::string();
    }
};

class JsonParser
{
public:
    explicit JsonParser(const std::string& aText) : p(aText.data()), end(aText.data() + aText.size()) {}

    bool Parse(Json& aOut) { return Value(aOut, 0); }

private:
    const char* p;
    const char* end;

    void Ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p; }

    static void PutUtf8(std::string& s, unsigned cp)
    {
        if (cp < 0x80) s += static_cast<char>(cp);
        else if (cp < 0x800) { s += static_cast<char>(0xC0 | (cp >> 6)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { s += static_cast<char>(0xE0 | (cp >> 12)); s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
        else { s += static_cast<char>(0xF0 | (cp >> 18)); s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
    }

    bool Hex4(unsigned& aOut)
    {
        if (end - p < 4) return false;
        aOut = 0;
        for (int i = 0; i < 4; ++i, ++p)
        {
            const char c = *p;
            aOut <<= 4;
            if (c >= '0' && c <= '9') aOut |= c - '0';
            else if (c >= 'a' && c <= 'f') aOut |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') aOut |= c - 'A' + 10;
            else return false;
        }
        return true;
    }

    bool String(std::string& aOut)
    {
        if (p >= end || *p != '"') return false;
        ++p;
        while (p < end && *p != '"')
        {
            if (*p != '\\') { aOut += *p++; continue; }
            if (++p >= end) return false;
            const char c = *p++;
            switch (c)
            {
            case 'n': aOut += '\n'; break;
            case 't': aOut += '\t'; break;
            case 'r': aOut += '\r'; break;
            case 'b': aOut += '\b'; break;
            case 'f': aOut += '\f'; break;
            case 'u':
            {
                unsigned cp;
                if (!Hex4(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u')
                {
                    p += 2;
                    unsigned lo;
                    if (!Hex4(lo)) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                PutUtf8(aOut, cp);
                break;
            }
            default: aOut += c; break;   // \" \\ \/
            }
        }
        if (p >= end) return false;
        ++p;
        return true;
    }

    bool Value(Json& aOut, int aDepth)
    {
        if (aDepth > 64) return false;
        Ws();
        if (p >= end) return false;
        if (*p == '{')
        {
            aOut.type = Json::Type::Object;
            ++p;
            Ws();
            if (p < end && *p == '}') { ++p; return true; }
            while (true)
            {
                Ws();
                std::string key;
                if (!String(key)) return false;
                Ws();
                if (p >= end || *p != ':') return false;
                ++p;
                Json v;
                if (!Value(v, aDepth + 1)) return false;
                aOut.fields.emplace_back(std::move(key), std::move(v));
                Ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == '}') { ++p; return true; }
                return false;
            }
        }
        if (*p == '[')
        {
            aOut.type = Json::Type::Array;
            ++p;
            Ws();
            if (p < end && *p == ']') { ++p; return true; }
            while (true)
            {
                Json v;
                if (!Value(v, aDepth + 1)) return false;
                aOut.items.push_back(std::move(v));
                Ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == ']') { ++p; return true; }
                return false;
            }
        }
        if (*p == '"')
        {
            aOut.type = Json::Type::String;
            return String(aOut.str);
        }
        // number, true, false, null: keep the raw text
        const char* start = p;
        while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\r' && *p != '\n' && *p != '\t') ++p;
        aOut.str.assign(start, p);
        aOut.type = aOut.str == "null" ? Json::Type::Null
                    : (aOut.str == "true" || aOut.str == "false") ? Json::Type::Bool : Json::Type::Number;
        return p > start;
    }
};

// ------------------------------------------------------------------ HTTP client (WinHTTP)
struct Http
{
    HINTERNET session = nullptr, connection = nullptr, request = nullptr;
    DWORD status = 0;
    ~Http()
    {
        if (request) WinHttpCloseHandle(request);
        if (connection) WinHttpCloseHandle(connection);
        if (session) WinHttpCloseHandle(session);
    }
};

// Sends a GET and reads the response headers. aExtraHeaders like L"Icy-MetaData: 1".
bool Open(Http& aHttp, const std::string& aUrl, int aTimeoutMs, const wchar_t* aExtraHeaders = nullptr)
{
    const std::wstring url = Wide(aUrl);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256]{};
    wchar_t path[4096]{};
    wchar_t extra[4096]{};
    uc.lpszHostName = host;
    uc.dwHostNameLength = static_cast<DWORD>(std::size(host));
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = static_cast<DWORD>(std::size(extra));
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc))
        return false;
    const bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;

    aHttp.session = WinHttpOpen(L"Mozilla/5.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
    if (!aHttp.session)
        return false;
    WinHttpSetTimeouts(aHttp.session, aTimeoutMs, aTimeoutMs, aTimeoutMs, aTimeoutMs);
    aHttp.connection = WinHttpConnect(aHttp.session, host, uc.nPort, 0);
    if (!aHttp.connection)
        return false;
    const std::wstring object = std::wstring(path) + extra;
    aHttp.request = WinHttpOpenRequest(aHttp.connection, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
    if (!aHttp.request)
        return false;
    if (!WinHttpSendRequest(aHttp.request, aExtraHeaders ? aExtraHeaders : WINHTTP_NO_ADDITIONAL_HEADERS,
                            aExtraHeaders ? static_cast<DWORD>(-1L) : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(aHttp.request, nullptr))
        return false;
    DWORD size = sizeof(aHttp.status);
    WinHttpQueryHeaders(aHttp.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &aHttp.status, &size, WINHTTP_NO_HEADER_INDEX);
    return true;
}

// reads exactly aCount bytes (false when the stream ends first)
bool ReadExact(Http& aHttp, char* aBuf, DWORD aCount)
{
    DWORD got = 0;
    while (got < aCount)
    {
        DWORD n = 0;
        if (!WinHttpReadData(aHttp.request, aBuf + got, aCount - got, &n) || n == 0)
            return false;
        got += n;
    }
    return true;
}

// body of a 200 response (at most 4 MB), empty otherwise
std::string Get(const std::string& aUrl, int aTimeoutMs)
{
    Http http;
    if (!Open(http, aUrl, aTimeoutMs) || http.status != 200)
        return {};
    std::string body;
    char buf[16384];
    while (body.size() < (4u << 20))
    {
        DWORD n = 0;
        if (!WinHttpReadData(http.request, buf, sizeof(buf), &n) || n == 0)
            break;
        body.append(buf, n);
    }
    return body;
}

// ------------------------------------------------------------------ find ffmpeg
// The game may be started by Steam with an old PATH, so look in the usual install places too.
std::wstring EnvVar(const wchar_t* aName)
{
    const DWORD n = GetEnvironmentVariableW(aName, nullptr, 0);
    if (n == 0)
        return {};
    std::wstring v(n, L'\0');
    v.resize(GetEnvironmentVariableW(aName, v.data(), n));
    return v;
}

std::wstring RegistryPath(HKEY aRoot, const wchar_t* aKey)
{
    wchar_t buf[32768];
    DWORD size = sizeof(buf);
    if (RegGetValueW(aRoot, aKey, L"Path", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, buf, &size) != ERROR_SUCCESS)
        return {};
    wchar_t expanded[32768];
    const DWORD n = ExpandEnvironmentStringsW(buf, expanded, static_cast<DWORD>(std::size(expanded)));
    return n > 0 && n <= std::size(expanded) ? std::wstring(expanded) : std::wstring(buf);
}

bool IsFile(const fs::path& aPath)
{
    std::error_code ec;
    return fs::is_regular_file(aPath, ec) || (fs::exists(aPath, ec) && !fs::is_directory(aPath, ec));
}

std::wstring FindFfmpeg(const fs::path& aPluginDir)
{
    if (IsFile(aPluginDir / L"ffmpeg.exe"))
        return (aPluginDir / L"ffmpeg.exe").wstring();
    const std::wstring paths = EnvVar(L"PATH") + L";" + RegistryPath(HKEY_CURRENT_USER, L"Environment") + L";" +
                               RegistryPath(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment");
    size_t start = 0;
    while (start <= paths.size())
    {
        size_t semi = paths.find(L';', start);
        if (semi == std::wstring::npos)
            semi = paths.size();
        std::wstring dir = paths.substr(start, semi - start);
        start = semi + 1;
        while (!dir.empty() && (dir.back() == L' ' || dir.back() == L'"'))
            dir.pop_back();
        while (!dir.empty() && (dir.front() == L' ' || dir.front() == L'"'))
            dir.erase(dir.begin());
        if (!dir.empty() && IsFile(fs::path(dir) / L"ffmpeg.exe"))
            return (fs::path(dir) / L"ffmpeg.exe").wstring();
    }
    const fs::path local = EnvVar(L"LOCALAPPDATA");
    const fs::path candidates[] = {
        local / L"Microsoft\\WinGet\\Links\\ffmpeg.exe",
        fs::path(EnvVar(L"USERPROFILE")) / L"scoop\\shims\\ffmpeg.exe",
        fs::path(EnvVar(L"ProgramData")) / L"chocolatey\\bin\\ffmpeg.exe",
        L"C:\\ffmpeg\\bin\\ffmpeg.exe",
        fs::path(EnvVar(L"ProgramFiles")) / L"ffmpeg\\bin\\ffmpeg.exe",
    };
    for (const auto& c : candidates)
        if (!c.empty() && IsFile(c))
            return c.wstring();
    std::error_code ec;
    const fs::path packages = local / L"Microsoft\\WinGet\\Packages";
    for (const auto& pkg : fs::directory_iterator(packages, ec))
    {
        if (!pkg.is_directory(ec) || pkg.path().filename().wstring().find(L"FFmpeg") == std::wstring::npos)
            continue;
        for (auto it = fs::recursive_directory_iterator(pkg.path(), ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            if (_wcsicmp(it->path().filename().c_str(), L"ffmpeg.exe") == 0)
                return it->path().wstring();
    }
    return {};
}

// ------------------------------------------------------------------ one connection = one ffmpeg for that station
bool SendAll(SOCKET aSocket, const char* aData, int aSize)
{
    while (aSize > 0)
    {
        const int n = send(aSocket, aData, aSize, 0);
        if (n <= 0)
            return false;
        aData += n;
        aSize -= n;
    }
    return true;
}

bool SendText(SOCKET aSocket, const std::string& aText) { return SendAll(aSocket, aText.data(), static_cast<int>(aText.size())); }

// which stream iHeart has for a station: plain stream, then .pls playlist (stations run by other companies), then HLS
void Resolve(const std::string& aId, std::string& aName, std::string& aSrc)
{
    aName = "iHeart " + aId;
    aSrc = "https://stream.revma.ihrhls.com/zc" + aId;
    const std::string body = Get("https://api.iheart.com/api/v2/content/liveStations/" + aId, 10000);
    Json root;
    if (body.empty() || !JsonParser(body).Parse(root))
    {
        Log("iHeart lookup failed for " + aId + ", trying " + aSrc);
        return;
    }
    const Json* hits = root.Get("hits");
    if (!hits || hits->type != Json::Type::Array || hits->items.empty())
        return;
    const Json& hit = hits->items[0];
    if (!hit.Str("name").empty())
        aName = hit.Str("name");
    const Json* streams = hit.Get("streams");
    if (!streams)
        return;
    std::string found = streams->Str("secure_shoutcast_stream");
    if (found.empty())
        found = streams->Str("shoutcast_stream");
    for (const char* key : {"secure_pls_stream", "pls_stream"})
    {
        const std::string pls = streams->Str(key);
        if (!found.empty() || pls.empty())
            continue;
        const std::string list = Get(pls, 10000);
        size_t start = 0;
        while (found.empty() && start < list.size())
        {
            size_t nl = list.find('\n', start);
            if (nl == std::string::npos)
                nl = list.size();
            const std::string line = Trim(list.substr(start, nl - start));
            start = nl + 1;
            // File1=http://...
            if (line.size() > 5 && _strnicmp(line.c_str(), "file", 4) == 0)
            {
                size_t i = 4;
                while (i < line.size() && isdigit(static_cast<unsigned char>(line[i])))
                    ++i;
                if (i > 4 && i < line.size() && line[i] == '=')
                    found = Trim(line.substr(i + 1));
            }
        }
    }
    if (found.empty())
        found = streams->Str("secure_hls_stream");
    if (found.empty())
        found = streams->Str("hls_stream");
    if (!found.empty())
        aSrc = found;
}

PROCESS_INFORMATION StartFfmpeg(const std::string& aSrc, HANDLE aStdout)
{
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + g_ffmpeg + L"\" -hide_banner -nostdin -loglevel error -reconnect 1 -reconnect_streamed 1 "
                       L"-reconnect_delay_max 5 -i \"" + Wide(aSrc) + L"\" -vn -c:a libmp3lame -b:a " +
                       std::to_wstring(kBitrate) + L"k -ar 44100 -write_xing 0 -id3v2_version 0 -f mp3 pipe:1";

    // hand ffmpeg only its output pipe, none of the game's other handles
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<char> attrBuf(attrSize);
    auto attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize))
        return pi;
    HANDLE inherit[] = {aStdout};
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdOutput = aStdout;
    si.lpAttributeList = attrs;
    if (!CreateProcessW(g_ffmpeg.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                        nullptr, nullptr, &si.StartupInfo, &pi))
        pi = {};
    DeleteProcThreadAttributeList(attrs);
    if (pi.hProcess && g_job)
        AssignProcessToJobObject(g_job, pi.hProcess);
    return pi;
}

void Handle(SOCKET aClient)
{
    // request line + headers (up to the blank line)
    DWORD timeout = 5000;
    setsockopt(aClient, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    std::string head;
    char c;
    while (head.size() < 8192 && recv(aClient, &c, 1, 0) == 1)
    {
        head += c;
        if (head.size() >= 4 && head.compare(head.size() - 4, 4, "\r\n\r\n") == 0)
            break;
    }
    const std::string request = head.substr(0, head.find('\n'));
    static const std::regex re(R"(^(GET|HEAD) /zc(\d+)(\.mp3)?[ ?])");
    std::smatch m;
    if (!std::regex_search(request, m, re))
    {
        SendText(aClient, "HTTP/1.0 404 Not Found\r\nConnection: close\r\n\r\nUse /zc<station id>.mp3\r\n");
        return;
    }
    const std::string method = m[1].str(), id = m[2].str();

    std::string name, src;
    Resolve(id, name, src);
    Log(method + " " + name + "  (" + src + ")");

    std::string safeName;
    for (char ch : name)
        if (ch >= 0x20 && ch <= 0x7E)
            safeName += ch;
    if (!SendText(aClient, "HTTP/1.0 200 OK\r\nContent-Type: audio/mpeg\r\nicy-name: " + safeName +
                               "\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n") ||
        method == "HEAD")
        return;

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 1 << 16))
    {
        Log("error: could not create a pipe for ffmpeg");
        return;
    }
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    PROCESS_INFORMATION pi = StartFfmpeg(src, writeEnd);
    CloseHandle(writeEnd);
    if (!pi.hProcess)
    {
        Log("error: could not start ffmpeg (" + std::to_string(GetLastError()) + ")");
        CloseHandle(readEnd);
        return;
    }
    {
        // this station is being listened to: the now-playing poller follows it while the connection lasts
        std::lock_guard lock(g_stateMutex);
        ++g_active[id];
        g_srcs[id] = src;
    }

    // runs until the game stops listening (send fails) or the stream ends
    std::vector<char> buf(16384);
    DWORD n = 0;
    while (!g_stop && ReadFile(readEnd, buf.data(), static_cast<DWORD>(buf.size()), &n, nullptr) && n > 0)
        if (!SendAll(aClient, buf.data(), static_cast<int>(n)))
            break;

    DWORD code = STILL_ACTIVE;
    GetExitCodeProcess(pi.hProcess, &code);
    if (code == STILL_ACTIVE)
        TerminateProcess(pi.hProcess, 0);   // the game switched away: this ffmpeg isn't needed any more
    else
        Log(name + " ended (ffmpeg exit " + std::to_string(code) + ")");
    CloseHandle(readEnd);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    std::lock_guard lock(g_stateMutex);
    if (--g_active[id] <= 0)
        g_active.erase(id);
}

void Serve()
{
    while (!g_stop)
    {
        const SOCKET client = accept(g_listen, nullptr, nullptr);
        if (client == INVALID_SOCKET)
        {
            if (g_stop)
                break;
            Sleep(50);
            continue;
        }
        std::thread([client] {
            try
            {
                Handle(client);
            }
            catch (...)
            {
                Log("error while serving a station");
            }
            closesocket(client);
        }).detach();
    }
}

// ------------------------------------------------------------------ now playing
// 1. the song titles sent inside the stream itself (ICY "StreamTitle", in step with the audio). iHeart sends:
//      StreamTitle='Aerosmith - text="What It Takes" song_spot="M" ...'          (M = music, T = jingle / talk)
//      StreamTitle='title="What Christmas Means To Me",artist="STEVIE WONDER",url="song_spot="F" ..."'
//    Returns true with artist / title (both empty when it's not a song), false when the stream sends no titles.
bool IcyTrack(const std::string& aUrl, std::string& aArtist, std::string& aTitle)
{
    if (aUrl.empty() || aUrl.find(".m3u8") != std::string::npos)
        return false;
    Http http;
    if (!Open(http, aUrl, 6000, L"Icy-MetaData: 1") || http.status != 200)
        return false;
    wchar_t value[32]{};
    DWORD size = sizeof(value);
    if (!WinHttpQueryHeaders(http.request, WINHTTP_QUERY_CUSTOM, L"icy-metaint", value, &size, WINHTTP_NO_HEADER_INDEX))
        return false;
    const int metaint = _wtoi(value);
    if (metaint <= 0 || metaint > (1 << 20))
        return false;
    std::vector<char> buf(metaint > 4096 ? metaint : 4096);
    for (int block = 0; block < 3; ++block)
    {
        // skip the audio before the metadata block, then read it (length byte x 16)
        unsigned char lenByte = 0;
        if (!ReadExact(http, buf.data(), metaint) || !ReadExact(http, reinterpret_cast<char*>(&lenByte), 1))
            return false;
        const int len = lenByte * 16;
        if (len == 0)
            continue;   // no title change in this block: try the next one
        if (!ReadExact(http, buf.data(), len))
            return false;
        std::string meta(buf.data(), len);
        meta.erase(meta.find_last_not_of('\0') + 1);
        static const std::regex streamTitle(R"(StreamTitle='([\s\S]*)';)");
        std::smatch m;
        if (!std::regex_search(meta, m, streamTitle))
            continue;
        const std::string t = m[1].str();
        static const std::regex titleArtist(R"re(title="([^"]*)"\s*,\s*artist="([^"]*)")re");
        static const std::regex artistText(R"re(^([\s\S]*?)\s+-\s+text="([^"]*)")re");
        static const std::regex plain(R"(^([\s\S]+?)\s+-\s+([\s\S]+)$)");
        static const std::regex spotRe(R"re(song_spot="([A-Za-z])")re");
        std::smatch x;
        aArtist.clear();
        aTitle.clear();
        if (std::regex_search(t, x, titleArtist)) { aTitle = x[1].str(); aArtist = x[2].str(); }
        else if (std::regex_search(t, x, artistText)) { aArtist = x[1].str(); aTitle = x[2].str(); }
        else if (t.find('=') == std::string::npos && std::regex_search(t, x, plain)) { aArtist = x[1].str(); aTitle = x[2].str(); }
        std::smatch s;
        const std::string spot = std::regex_search(t, s, spotRe) ? s[1].str() : std::string();
        if (spot == "T" || Trim(aArtist).empty() || Trim(aTitle).empty())
            aArtist.clear(), aTitle.clear();
        return true;
    }
    return false;
}

// 2. fallback when the stream sends no titles: iHeart's live-meta API (gaps of many minutes on some stations)
void ApiTrack(const std::string& aId, std::string& aArtist, std::string& aTitle)
{
    aArtist.clear();
    aTitle.clear();
    const std::string body = Get("https://api.iheart.com/api/v3/live-meta/stream/" + aId + "/currentTrackMeta", 5000);
    Json j;
    if (!body.empty() && JsonParser(body).Parse(j))   // 204 / 410 (nothing on air right now) give an empty body
    {
        aArtist = j.Str("artist");
        aTitle = j.Str("title");
    }
}

void Poll(const std::vector<std::string>& aIds, const std::map<std::string, std::string>& aSrcs)
{
    std::string lines;
    for (const auto& id : aIds)
    {
        std::string artist, title;
        const auto src = aSrcs.find(id);
        if (!IcyTrack(src != aSrcs.end() ? src->second : std::string(), artist, title))
            ApiTrack(id, artist, title);
        artist = Clean(artist);
        title = Clean(title);
        if (!lines.empty())
            lines += '\n';
        lines += id + "\t" + artist + "\t" + title;
        const std::string text = title.empty() ? "(no song info: ad or DJ)" : artist + " - " + title;
        if (g_lastTrack[id] != text)
        {
            g_lastTrack[id] = text;
            Log("now playing on " + id + " : " + text);
        }
    }
    if (g_nowFile.empty())
        return;
    const fs::path tmp = g_nowFile.wstring() + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << lines;
    }
    if (!MoveFileExW(tmp.c_str(), g_nowFile.c_str(), MOVEFILE_REPLACE_EXISTING))
        Log("could not write now playing file (" + std::to_string(GetLastError()) + ")");
}

// every 10 s, or at once when the set of listened stations changes
void PollLoop()
{
    ULONGLONG last = 0;
    std::string lastKey = "-";
    while (!g_stop)
    {
        std::vector<std::string> ids;
        std::map<std::string, std::string> srcs;
        {
            std::lock_guard lock(g_stateMutex);
            for (const auto& [id, n] : g_active)
                ids.push_back(id);
            srcs = g_srcs;
        }
        std::string key;
        for (const auto& id : ids)
            key += id + ",";
        if (GetTickCount64() - last >= 10000 || key != lastKey)
        {
            last = GetTickCount64();
            lastKey = key;
            try
            {
                Poll(ids, srcs);
            }
            catch (...)
            {
                Log("now playing error");
            }
        }
        Sleep(250);
    }
}
} // namespace

namespace Bridge
{
bool Start(const fs::path& aPluginDir, std::string& aError)
{
    g_log = aPluginDir / L"bridge.log";
    {
        std::ofstream reset(g_log, std::ios::binary | std::ios::trunc);
    }

    g_ffmpeg = FindFfmpeg(aPluginDir);
    if (g_ffmpeg.empty())
    {
        aError = "ffmpeg was not found. Install it with the Station Manager (INSTALL FFMPEG), or: winget install Gyan.FFmpeg";
        Log(aError);
        return false;
    }

    // the CET part of the mod: the game folder is three levels above red4ext\plugins\iHeartRadio
    std::error_code ec;
    const fs::path cetMods = aPluginDir.parent_path().parent_path().parent_path() / L"bin\\x64\\plugins\\cyber_engine_tweaks\\mods";
    if (fs::is_directory(cetMods, ec))
    {
        const fs::path data = cetMods / L"iHeartRadio\\data";
        fs::create_directories(data, ec);
        g_nowFile = data / L"nowplaying.txt";
        std::ofstream reset(g_nowFile, std::ios::binary | std::ios::trunc);
    }
    else
        Log("CET mods folder not found: song titles won't show in game");

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
    {
        aError = "could not start networking";
        Log(aError);
        return false;
    }
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL exclusive = TRUE;
    setsockopt(g_listen, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (g_listen == INVALID_SOCKET || bind(g_listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(g_listen, SOMAXCONN) != 0)
    {
        // most likely a second game instance: the first one serves
        aError = "port " + std::to_string(kPort) + " is already in use (bridge already running?)";
        Log(aError);
        if (g_listen != INVALID_SOCKET)
            closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        return false;
    }

    // ffmpeg processes go in a job that Windows closes together with the game
    g_job = CreateJobObjectW(nullptr, nullptr);
    if (g_job)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }

    Log("iHeart Radio bridge running on http://127.0.0.1:" + std::to_string(kPort) + "/zc<id>.mp3  (ffmpeg: " +
        Utf8(g_ffmpeg) + ")");
    std::thread(Serve).detach();
    std::thread(PollLoop).detach();
    return true;
}

void Stop()
{
    g_stop = true;
    if (g_listen != INVALID_SOCKET)
    {
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
    }
    if (g_job)
    {
        TerminateJobObject(g_job, 0);
        CloseHandle(g_job);
        g_job = nullptr;
    }
}
} // namespace Bridge
