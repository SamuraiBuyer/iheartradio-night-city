// iHeart Radio - small helpers shared by the bridge and the station manager: text, the log, JSON, HTTP (WinHTTP).
#pragma once

#include <winsock2.h>
#include <Windows.h>
#include <winhttp.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace Net
{
// ------------------------------------------------------------------ text
std::string Utf8(const std::wstring& aText);
std::wstring Wide(const std::string& aText);
std::string Trim(const std::string& s);
std::string Lower(std::string s);
void ReplaceAll(std::string& s, const std::string& from, const std::string& to);
// typographic quotes / dashes -> plain ones (safer for the game's fonts), no tabs / line breaks
std::string Clean(std::string s);
std::string UrlEncode(const std::string& s);

// ------------------------------------------------------------------ log (red4ext\plugins\iHeartRadio\bridge.log)
void SetLog(const std::filesystem::path& aFile);   // starts it over
void Log(const std::string& aText);

// ------------------------------------------------------------------ minimal JSON
struct Json
{
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    std::string str;   // strings; numbers / true / false as written
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> fields;

    const Json* Get(const char* aKey) const;
    std::string Str(const char* aKey) const;    // string or number field as text, "" when missing
    double Num(const char* aKey, double aDefault = 0) const;
    bool Bool(const char* aKey, bool aDefault = false) const;
};
bool ParseJson(const std::string& aText, Json& aOut);
std::string Quote(const std::string& s);   // JSON string literal

// ------------------------------------------------------------------ HTTP
struct Http
{
    HINTERNET session = nullptr, connection = nullptr, request = nullptr;
    DWORD status = 0;
    ~Http();
};
// Sends a GET and reads the response headers. aExtraHeaders like L"Icy-MetaData: 1".
bool Open(Http& aHttp, const std::string& aUrl, int aTimeoutMs, const wchar_t* aExtraHeaders = nullptr);
bool ReadExact(Http& aHttp, char* aBuf, DWORD aCount);   // false when the stream ends first
std::string ContentType(Http& aHttp);                     // lower case
std::string FinalUrl(Http& aHttp, const std::string& aRequested);   // after redirects
// body of a 200 response (at most 4 MB), empty otherwise
std::string Get(const std::string& aUrl, int aTimeoutMs, std::string* aFinalUrl = nullptr, std::string* aType = nullptr);
} // namespace Net
