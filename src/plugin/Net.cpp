// iHeart Radio - shared helpers (see Net.hpp).
#include "Net.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>

namespace Net
{
// ------------------------------------------------------------------ text
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

std::string Trim(const std::string& s)
{
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string Lower(std::string s)
{
    for (auto& c : s)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

void ReplaceAll(std::string& s, const std::string& from, const std::string& to)
{
    for (size_t p = s.find(from); p != std::string::npos; p = s.find(from, p + to.size()))
        s.replace(p, from.size(), to);
}

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

std::string UrlEncode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s)
    {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += static_cast<char>(c);
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

// ------------------------------------------------------------------ log
namespace
{
std::filesystem::path g_log;
std::mutex g_logMutex;
} // namespace

void SetLog(const std::filesystem::path& aFile)
{
    std::lock_guard lock(g_logMutex);
    g_log = aFile;
    std::ofstream reset(g_log, std::ios::binary | std::ios::trunc);
}

void Log(const std::string& aText)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[32];
    sprintf_s(stamp, "[%04u-%02u-%02u %02u:%02u:%02u] ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::lock_guard lock(g_logMutex);
    if (g_log.empty())
        return;
    std::ofstream f(g_log, std::ios::app | std::ios::binary);
    f << stamp << aText << "\r\n";
}

// ------------------------------------------------------------------ JSON
const Json* Json::Get(const char* aKey) const
{
    if (type != Type::Object)
        return nullptr;
    for (const auto& [k, v] : fields)
        if (k == aKey)
            return &v;
    return nullptr;
}

std::string Json::Str(const char* aKey) const
{
    const Json* v = Get(aKey);
    return v && (v->type == Type::String || v->type == Type::Number) ? v->str : std::string();
}

double Json::Num(const char* aKey, double aDefault) const
{
    const Json* v = Get(aKey);
    if (!v || (v->type != Type::Number && v->type != Type::String) || v->str.empty())
        return aDefault;
    char* end = nullptr;
    const double d = strtod(v->str.c_str(), &end);
    return end && *end == '\0' ? d : aDefault;
}

bool Json::Bool(const char* aKey, bool aDefault) const
{
    const Json* v = Get(aKey);
    return v && v->type == Type::Bool ? v->str == "true" : aDefault;
}

namespace
{
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
} // namespace

bool ParseJson(const std::string& aText, Json& aOut)
{
    aOut = Json();
    return JsonParser(aText).Parse(aOut);
}

std::string Quote(const std::string& s)
{
    std::string out = "\"";
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                sprintf_s(buf, "\\u%04x", c);
                out += buf;
            }
            else
                out += static_cast<char>(c);
        }
    }
    return out + "\"";
}

// ------------------------------------------------------------------ HTTP
Http::~Http()
{
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);
}

bool Open(Http& aHttp, const std::string& aUrl, int aTimeoutMs, const wchar_t* aExtraHeaders)
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

std::string ContentType(Http& aHttp)
{
    wchar_t buf[512]{};
    DWORD size = sizeof(buf);
    if (!WinHttpQueryHeaders(aHttp.request, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, buf, &size,
                             WINHTTP_NO_HEADER_INDEX))
        return {};
    return Lower(Utf8(buf));
}

std::string FinalUrl(Http& aHttp, const std::string& aRequested)
{
    DWORD size = 0;
    WinHttpQueryOption(aHttp.request, WINHTTP_OPTION_URL, nullptr, &size);
    if (size == 0)
        return aRequested;
    std::wstring url(size / sizeof(wchar_t) + 1, L'\0');
    if (!WinHttpQueryOption(aHttp.request, WINHTTP_OPTION_URL, url.data(), &size))
        return aRequested;
    url.resize(wcslen(url.c_str()));
    return url.empty() ? aRequested : Utf8(url);
}

std::string Get(const std::string& aUrl, int aTimeoutMs, std::string* aFinalUrl, std::string* aType)
{
    Http http;
    if (!Open(http, aUrl, aTimeoutMs) || http.status != 200)
        return {};
    if (aFinalUrl)
        *aFinalUrl = FinalUrl(http, aUrl);
    if (aType)
        *aType = ContentType(http);
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
} // namespace Net
