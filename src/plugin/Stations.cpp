// iHeart Radio - the station manager inside the game (see Stations.hpp). Same files and rules as the app's Backend.cs.
#include "Stations.hpp"
#include "IconArchive.hpp"
#include "Logo.hpp"
#include "Net.hpp"

#pragma warning(push, 0)
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>
#pragma warning(pop)

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace Net;

namespace Stations
{
namespace
{
constexpr const char* kVersion = "2.1.0";
constexpr int kBridgePort = 8925;
constexpr const char* kLogoFormat = "3";   // logos_format.txt: archive layout (3 = uncompressed), same as the app

fs::path g_game, g_radios, g_data, g_logos, g_archive, g_next, g_pendingFlag, g_formatStamp;
std::mutex g_files;   // one writer at a time (installs, removes, the archive)

// ------------------------------------------------------------------ small helpers
std::string ReadFile(const fs::path& aPath)
{
    std::ifstream f(aPath, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

// writes through a temporary file so a half-written file never replaces a good one
bool WriteFile(const fs::path& aPath, const std::string& aData)
{
    std::error_code ec;
    fs::create_directories(aPath.parent_path(), ec);
    const fs::path tmp = aPath.wstring() + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << aData;
        if (!f)
            return false;
    }
    if (MoveFileExW(tmp.c_str(), aPath.c_str(), MOVEFILE_REPLACE_EXISTING))
        return true;
    DeleteFileW(tmp.c_str());   // the target is in use (the game's open archive)
    return false;
}

bool IsFile(const fs::path& aPath)
{
    std::error_code ec;
    return fs::is_regular_file(aPath, ec);
}

std::string BaseImage(const std::string& aUrl) { return aUrl.substr(0, aUrl.find('?')); }

std::string Num(double v)   // like C#'s ToString("0.0##")
{
    char buf[64];
    sprintf_s(buf, "%.3f", v);
    std::string s = buf;
    while (s.size() > 1 && s.back() == '0' && s[s.size() - 2] != '.')
        s.pop_back();
    return s;
}

std::string Error(const std::string& aMessage) { return "{\"ok\":false,\"error\":" + Quote(aMessage) + "}"; }

bool ValidId(const std::string& aId) { return std::regex_match(aId, std::regex(R"(\d{1,8})")); }

// ------------------------------------------------------------------ installed stations: radioExt\radios\iheart_<id>_<name>
std::map<std::string, fs::path> Installed()
{
    std::map<std::string, fs::path> out;
    std::error_code ec;
    static const std::regex re(R"(^iheart_(\d+))");
    for (const auto& d : fs::directory_iterator(g_radios, ec))
    {
        std::smatch m;
        const std::string name = Utf8(d.path().filename().wstring());
        if (d.is_directory(ec) && std::regex_search(name, m, re) && IsFile(d.path() / L"metadata.json"))
            out[m[1].str()] = d.path();
    }
    return out;
}

fs::path LogoPng(const std::string& aId) { return g_logos / Wide(aId + ".png"); }
fs::path LogoJson(const std::string& aId) { return g_logos / Wide(aId + ".json"); }

// 512x512 alpha of a saved logo (white on transparent PNG), top row first; empty when there's none
std::vector<uint8_t> LoadLogo(const std::string& aId)
{
    const std::string png = ReadFile(LogoPng(aId));
    if (png.empty())
        return {};
    int w = 0, h = 0, c = 0;
    unsigned char* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(png.data()), static_cast<int>(png.size()), &w, &h, &c, 4);
    if (!px)
        return {};
    const int S = IconArchive::kIconSize;
    std::vector<uint8_t> a(static_cast<size_t>(S) * S);
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++)
            a[static_cast<size_t>(y) * S + x] = px[(static_cast<size_t>(y * h / S) * w + x * w / S) * 4 + 3];
    stbi_image_free(px);
    return a;
}

bool SaveLogo(const std::string& aId, const std::vector<uint8_t>& aAlpha)
{
    const int S = IconArchive::kIconSize;
    std::vector<uint8_t> rgba(static_cast<size_t>(S) * S * 4, 255);
    for (size_t i = 0; i < aAlpha.size(); i++)
        rgba[i * 4 + 3] = aAlpha[i];
    std::string png;
    stbi_write_png_to_func([](void* ctx, void* data, int size) { static_cast<std::string*>(ctx)->append(static_cast<char*>(data), size); },
                           &png, S, S, 4, rgba.data(), S * 4);
    return !png.empty() && WriteFile(LogoPng(aId), png);
}

// ------------------------------------------------------------------ the logo archive
bool LogosOutdated()
{
    std::error_code ec;
    bool any = false;
    for (const auto& f : fs::directory_iterator(g_logos, ec))
        if (f.path().extension() == L".png")
            any = true;
    return any && Trim(ReadFile(g_formatStamp)) != kLogoFormat;
}

// Rebuilds the archive from the saved logos. Returns true when it's in place now, false when the game has it open
// (then it waits as .archive.next for the next start).
bool ApplyLogos(std::string& aError)
{
    std::vector<std::pair<long long, std::vector<uint8_t>>> icons;
    for (const auto& [id, folder] : Installed())
    {
        auto alpha = LoadLogo(id);
        if (!alpha.empty())
            icons.emplace_back(std::stoll(id), std::move(alpha));
    }
    std::string data;
    if (!icons.empty() && !IconArchive::Build(icons, data, aError))
        return false;
    std::error_code ec;
    fs::create_directories(g_data, ec);
    bool done;
    if (icons.empty())
        done = !fs::exists(g_archive, ec) || DeleteFileW(g_archive.c_str());
    else
    {
        fs::create_directories(g_archive.parent_path(), ec);
        done = WriteFile(g_archive, data);
    }
    if (done)
    {
        fs::remove(g_next, ec);
        fs::remove(g_pendingFlag, ec);
        WriteFile(g_formatStamp, kLogoFormat);
        return true;
    }
    // the game keeps the archive open: an empty .next means "no logos", so delete it
    WriteFile(g_next, data);
    WriteFile(g_pendingFlag, "waiting for the game to restart");
    return false;
}

// ------------------------------------------------------------------ iHeart
// the game's layout: frequency first, like "89.3 Radio Vexelstrom" ("Power 105.1" -> "105.1 Power")
std::string GameName(const std::string& aName, const std::string& aFreq, const std::string& aCall)
{
    if (aFreq.empty())
        return aName;
    std::string rest;
    for (size_t i = 0; i < aName.size();)
    {
        const bool at = aName.compare(i, aFreq.size(), aFreq) == 0;
        const bool before = i > 0 && (isalnum(static_cast<unsigned char>(aName[i - 1])) || aName[i - 1] == '_' || aName[i - 1] == '.');
        const size_t e = i + aFreq.size();
        const bool after = e < aName.size() && (isdigit(static_cast<unsigned char>(aName[e])) || aName[e] == '.');
        if (at && !before && !after)   // "Q104.3" stays whole instead of becoming "104.3 Q"
        {
            i = e;
            continue;
        }
        rest += aName[i++];
    }
    rest = std::regex_replace(rest, std::regex(R"(\s+)"), " ");
    const size_t b = rest.find_first_not_of(" -/"), en = rest.find_last_not_of(" -/");
    rest = b == std::string::npos ? std::string() : rest.substr(b, en - b + 1);
    return aFreq + " " + (rest.empty() ? aCall : rest);
}

std::string Search(const std::string& aQuery)
{
    const std::string q = Trim(aQuery);
    if (q.empty())
        return "{\"ok\":true,\"stations\":[]}";
    const std::string body = Get("https://api.iheart.com/api/v3/search/all?keywords=" + UrlEncode(q) +
                                     "&maxRows=24&station=true&artist=false&track=false&playlist=false&podcast=false&keyword=false&bundle=false",
                                 10000);
    Json root;
    if (body.empty() || !ParseJson(body, root))
        return Error("Couldn't reach iHeartRadio. Check your internet connection.");
    const Json* results = root.Get("results");
    const Json* list = results ? results->Get("stations") : nullptr;
    const auto installed = Installed();
    std::string out = "{\"ok\":true,\"stations\":[";
    bool first = true;
    if (list)
        for (const auto& s : list->items)
        {
            const std::string id = s.Str("id");
            if (!ValidId(id))
                continue;
            out += std::string(first ? "" : ",") + "{\"id\":" + Quote(id) + ",\"name\":" + Quote(Clean(s.Str("name"))) +
                   ",\"callLetters\":" + Quote(s.Str("callLetters")) + ",\"frequency\":" + Quote(s.Str("frequency")) +
                   ",\"description\":" + Quote(Clean(s.Str("description"))) + ",\"genre\":" + Quote(s.Str("genre")) +
                   ",\"image\":" + Quote(BaseImage(s.Str("imageUrl"))) +
                   ",\"installed\":" + (installed.count(id) ? "true" : "false") + "}";
            first = false;
        }
    return out + "]}";
}

// the station being set up in the menu: its logo analysed once, styles redone when the line gap changes
struct Current
{
    std::string id;
    int gap = 3;
    bool hasLogo = false;
    Logo::Analysis an;
    std::vector<Logo::Candidate> styles;
};
std::mutex g_currentMutex;
Current g_current;

std::string StylesJson(const Current& aCur)
{
    std::string out = "[";
    for (size_t i = 0; i < aCur.styles.size(); i++)
        out += std::string(i ? "," : "") + "{\"key\":" + Quote(aCur.styles[i].key) + ",\"label\":" + Quote(aCur.styles[i].label) + "}";
    return out + "]";
}

std::string Station(const std::string& aId, int aGap)
{
    if (!ValidId(aId))
        return Error("bad station id");
    const std::string body = Get("https://api.iheart.com/api/v2/content/liveStations/" + aId, 10000);
    Json root;
    if (body.empty() || !ParseJson(body, root))
        return Error("Couldn't reach iHeartRadio. Check your internet connection.");
    const Json* hits = root.Get("hits");
    if (!hits || hits->items.empty())
        return Error("iHeart has no station with id " + aId);
    const Json& h = hits->items[0];
    const std::string name = Clean(h.Str("name")), freq = h.Str("freq");
    const std::string call = std::regex_replace(h.Str("callLetters"), std::regex("-(FM|AM)$"), "");
    double fm = 0;
    {
        const std::string digits = std::regex_replace(freq, std::regex(R"([^0-9.])"), "");
        if (!digits.empty())
            fm = strtod(digits.c_str(), nullptr);
    }
    std::string city;
    if (const Json* markets = h.Get("markets"); markets && !markets->items.empty())
    {
        city = markets->items[0].Str("city");
        const std::string st = markets->items[0].Str("stateAbbreviation");
        if (!st.empty())
            city += ", " + st;
    }
    const Json* streams = h.Get("streams");
    const bool hasStream = streams && !streams->fields.empty();

    // logo: the big PNG version, analysed for the styles
    Current cur;
    cur.id = aId;
    cur.gap = aGap;
    const std::string logoUrl = h.Str("logo");
    if (!logoUrl.empty())
    {
        Logo::Image img;
        const std::string png = Get(BaseImage(logoUrl) + "?ops=fit(1024,1024),format(%22png%22)", 15000);
        if (!png.empty() && Logo::Decode(png, img))
        {
            cur.an = Logo::Analyze(img);
            cur.styles = Logo::Candidates(cur.an, aGap);
            cur.hasLogo = !cur.styles.empty();
        }
    }
    const std::string styles = StylesJson(cur);
    const bool hasLogo = cur.hasLogo;
    {
        std::lock_guard lock(g_currentMutex);
        g_current = std::move(cur);
    }

    // already installed: what it's set to now
    std::string installed = "null";
    const auto all = Installed();
    if (const auto it = all.find(aId); it != all.end())
    {
        Json meta;
        ParseJson(ReadFile(it->second / L"metadata.json"), meta);
        Json settings;
        const bool hasSettings = ParseJson(ReadFile(LogoJson(aId)), settings);
        installed = "{\"displayName\":" + Quote(meta.Str("displayName")) + ",\"fm\":" + Num(meta.Num("fm")) +
                    ",\"volume\":" + Num(meta.Num("volume", 1)) + ",\"hasLogo\":" + (IsFile(LogoPng(aId)) ? "true" : "false") +
                    ",\"key\":" + Quote(hasSettings ? settings.Str("key") : "") + ",\"gap\":" +
                    std::to_string(hasSettings ? static_cast<int>(settings.Num("gap", 3)) : 3) +
                    ",\"invert\":" + (hasSettings && settings.Bool("invert") ? "true" : "false") +
                    ",\"source\":" + Quote(hasSettings ? settings.Str("source") : "") + "}";
    }
    return "{\"ok\":true,\"id\":" + Quote(aId) + ",\"name\":" + Quote(name) + ",\"callLetters\":" + Quote(call) +
           ",\"freq\":" + Quote(freq) + ",\"city\":" + Quote(city) + ",\"description\":" + Quote(Clean(h.Str("description"))) +
           ",\"displayName\":" + Quote(GameName(name, freq, call)) + ",\"fm\":" + Num(fm) +
           ",\"hasStream\":" + (hasStream ? "true" : "false") + ",\"hasLogo\":" + (hasLogo ? "true" : "false") +
           ",\"styles\":" + styles + ",\"installed\":" + installed + "}";
}

std::string Restyle(int aGap)
{
    std::lock_guard lock(g_currentMutex);
    if (!g_current.hasLogo)
        return Error("no logo loaded");
    if (aGap != g_current.gap)
    {
        g_current.gap = aGap;
        g_current.styles = Logo::Candidates(g_current.an, aGap);
    }
    return "{\"ok\":true,\"gap\":" + std::to_string(aGap) + ",\"styles\":" + StylesJson(g_current) + "}";
}

// A small preview of a style of station aId's logo (or "current": its installed logo) as hex, one digit per pixel
// (alpha / 16), top row first. {"ok":false,"busy":true} while the styles for that station / line gap are being made.
std::string Preview(const std::string& aId, const std::string& aKey, bool aInvert, int aGap, int aSize)
{
    aSize = std::clamp(aSize, 16, 160);
    std::vector<uint8_t> alpha;
    {
        std::lock_guard lock(g_currentMutex);
        if (aKey != "current" && (g_current.id != aId || g_current.gap != aGap))
            return "{\"ok\":false,\"busy\":true}";
        if (aKey == "current")
        {
            if (!ValidId(aId))
                return "{\"ok\":false}";
            const auto big = LoadLogo(aId);
            if (!big.empty())
            {
                const int S = IconArchive::kIconSize;
                alpha.resize(static_cast<size_t>(aSize) * aSize);
                for (int y = 0; y < aSize; y++)
                    for (int x = 0; x < aSize; x++)
                    {
                        // box average
                        int acc = 0, n = 0;
                        for (int yy = y * S / aSize; yy < (y + 1) * S / aSize; yy++)
                            for (int xx = x * S / aSize; xx < (x + 1) * S / aSize; xx++, n++)
                                acc += big[static_cast<size_t>(yy) * S + xx];
                        alpha[static_cast<size_t>(y) * aSize + x] = static_cast<uint8_t>(n ? acc / n : 0);
                    }
            }
        }
        else
            for (const auto& s : g_current.styles)
                if (s.key == aKey)
                    alpha = Logo::Fit(s.ink, g_current.an.w, g_current.an.h, aSize, aInvert);
    }
    if (alpha.empty())
        return "{\"ok\":false}";
    static const char* hex = "0123456789abcdef";
    std::string px;
    px.reserve(alpha.size());
    for (uint8_t a : alpha)
        px += hex[a >> 4];
    return "{\"ok\":true,\"size\":" + std::to_string(aSize) + ",\"pixels\":\"" + px + "\"}";
}

// aIconId: the station id when it has a logo, -1 for the game's default icon
// A station's own (colour) logo, small, on white like the app's search cards: hex, 3 digits per pixel (4-bit R G B),
// top row first. Only iHeart's image server.
std::string Thumb(const std::string& aUrl, int aSize)
{
    aSize = std::clamp(aSize, 16, 96);
    if (aUrl.rfind("https://i.iheart.com/", 0) != 0)
        return Error("not an iHeart image");
    Logo::Image img;
    const std::string bytes = Get(BaseImage(aUrl) + "?ops=fit(160,160)", 10000);
    if (bytes.empty() || !Logo::Decode(bytes, img, aSize))
        return Error("no image");
    static const char* hex = "0123456789abcdef";
    std::string px(static_cast<size_t>(aSize) * aSize * 3, 'f');   // white
    const int ox = (aSize - img.w) / 2, oy = (aSize - img.h) / 2;
    for (int y = 0; y < img.h; y++)
        for (int x = 0; x < img.w; x++)
        {
            const uint8_t* p = &img.rgba[(static_cast<size_t>(y) * img.w + x) * 4];
            const double a = p[3] / 255.0;
            const size_t o = (static_cast<size_t>(y + oy) * aSize + (x + ox)) * 3;
            for (int c = 0; c < 3; c++)
            {
                const int v = static_cast<int>(p[c] * a + 255 * (1 - a) + 0.5);   // over white
                px[o + c] = hex[std::min(15, (v + 8) / 17)];
            }
        }
    return "{\"ok\":true,\"size\":" + std::to_string(aSize) + ",\"pixels\":\"" + px + "\"}";
}

void WriteMetadata(const fs::path& aFile, const std::string& aId, const std::string& aName, double aFm, double aVolume, long long aIconId)
{
    const std::string url = "http://127.0.0.1:" + std::to_string(kBridgePort) + "/zc" + aId + ".ogg";   // Ogg Vorbis
    std::string s = "{\n";
    s += "    \"displayName\": " + Quote(aName) + ",\n";
    s += "    \"fm\": " + Num(aFm) + ",\n";
    s += "    \"volume\": " + Num(aVolume) + ",\n";
    s += "    \"icon\": \"default\",\n";
    s += "    \"customIcon\": {\n";
    s += "        \"inkAtlasPath\": " + Quote(aIconId >= 0 ? IconArchive::AtlasPath(aIconId) : "") + ",\n";
    s += "        \"inkAtlasPart\": " + Quote(aIconId >= 0 ? IconArchive::IconName(aIconId) : "") + ",\n";
    s += std::string("        \"useCustom\": ") + (aIconId >= 0 ? "true" : "false") + "\n";
    s += "    },\n";
    s += "    \"streamInfo\": {\n";
    s += "        \"streamURL\": " + Quote(url) + ",\n";
    s += "        \"isStream\": true\n";
    s += "    },\n";
    s += "    \"order\": []\n";
    s += "}\n";
    WriteFile(aFile, s);
}

// {"op":"install","id":"1469","displayName":"100.3 Z100","fm":100.3,"volume":1,"callLetters":"WHTZ",
//  "logo":"style"|"keep"|"none","key":"solid","gap":3,"invert":false}
std::string Install(const Json& p)
{
    const std::string id = p.Str("id");
    if (!ValidId(id))
        return Error("bad station id");
    const std::string name = Trim(p.Str("displayName"));
    if (name.empty())
        return Error("The station needs a name.");
    const double fm = p.Num("fm", 0);
    const double volume = std::clamp(p.Num("volume", 1), 0.0, 2.0);
    const std::string logo = p.Str("logo");

    std::lock_guard files(g_files);
    if (logo == "style")
    {
        std::vector<uint8_t> alpha;
        const int gap = static_cast<int>(p.Num("gap", 3));
        const std::string key = p.Str("key");
        const bool invert = p.Bool("invert");
        {
            std::lock_guard lock(g_currentMutex);
            if (g_current.id != id || !g_current.hasLogo)
                return Error("The logo isn't loaded any more. Open the station again.");
            if (gap != g_current.gap)
            {
                g_current.gap = gap;
                g_current.styles = Logo::Candidates(g_current.an, gap);
            }
            for (const auto& s : g_current.styles)
                if (s.key == key)
                    alpha = Logo::Fit(s.ink, g_current.an.w, g_current.an.h, IconArchive::kIconSize, invert);
        }
        if (alpha.empty() || !SaveLogo(id, alpha))
            return Error("Couldn't save the logo.");
        WriteFile(LogoJson(id), "{\"key\":" + Quote(key) + ",\"gap\":" + std::to_string(gap) + ",\"invert\":" +
                                    (invert ? "true" : "false") + ",\"source\":\"iheart\"}");
    }
    else if (logo == "none")
    {
        std::error_code ec;
        fs::remove(LogoPng(id), ec);
        fs::remove(LogoJson(id), ec);
    }

    const auto all = Installed();
    fs::path folder;
    if (const auto it = all.find(id); it != all.end())
        folder = it->second;
    else
    {
        std::string slug = std::regex_replace(p.Str("callLetters").empty() ? name : p.Str("callLetters"), std::regex("[^A-Za-z0-9]+"), "_");
        const size_t b = slug.find_first_not_of('_'), e = slug.find_last_not_of('_');
        slug = b == std::string::npos ? std::string() : Lower(slug.substr(b, e - b + 1));
        folder = g_radios / Wide("iheart_" + id + "_" + (slug.empty() ? "station" : slug));
    }
    std::error_code ec;
    fs::create_directories(folder, ec);
    const long long stationId = std::stoll(id);
    WriteMetadata(folder / L"metadata.json", id, name, fm, volume, IsFile(LogoPng(id)) ? stationId : -1);
    std::string err;
    const bool now = ApplyLogos(err);
    Log("station " + id + " (" + name + ") saved" + (err.empty() ? "" : ", logo problem: " + err));
    return "{\"ok\":true,\"id\":" + Quote(id) + ",\"logosApplied\":" + (now ? "true" : "false") + "}";
}

std::string Remove(const std::string& aId)
{
    std::lock_guard files(g_files);
    std::error_code ec;
    for (const auto& [id, folder] : Installed())
        if (id == aId)
            fs::remove_all(folder, ec);
    fs::remove(LogoPng(aId), ec);
    fs::remove(LogoJson(aId), ec);
    std::string err;
    const bool now = ApplyLogos(err);
    Log("station " + aId + " removed");
    return std::string("{\"ok\":true,\"logosApplied\":") + (now ? "true" : "false") + "}";
}

std::string RemoveAll()
{
    std::lock_guard files(g_files);
    std::error_code ec;
    for (const auto& [id, folder] : Installed())
        fs::remove_all(folder, ec);
    fs::remove_all(g_logos, ec);
    std::string err;
    const bool now = ApplyLogos(err);
    Log("all stations removed");
    return std::string("{\"ok\":true,\"logosApplied\":") + (now ? "true" : "false") + "}";
}

std::string List()
{
    std::vector<std::pair<double, std::string>> rows;
    for (const auto& [id, folder] : Installed())
    {
        Json meta;
        if (!ParseJson(ReadFile(folder / L"metadata.json"), meta))
            continue;
        const double fm = meta.Num("fm");
        rows.emplace_back(fm, "{\"id\":" + Quote(id) + ",\"displayName\":" + Quote(meta.Str("displayName")) + ",\"fm\":" + Num(fm) +
                                  ",\"volume\":" + Num(meta.Num("volume", 1)) + ",\"hasLogo\":" + (IsFile(LogoPng(id)) ? "true" : "false") +
                                  ",\"folder\":" + Quote(Utf8(folder.filename().wstring())) + "}");
    }
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::string out = "{\"ok\":true,\"stations\":[";
    for (size_t i = 0; i < rows.size(); i++)
        out += (i ? "," : "") + rows[i].second;
    return out + "]}";
}

std::string Status()
{
    std::error_code ec;
    const bool radioExt = IsFile(g_game / L"red4ext\\plugins\\RadioExt\\RadioExt.dll");
    return std::string("{\"ok\":true,\"version\":\"") + kVersion + "\",\"radioExt\":" + (radioExt ? "true" : "false") +
           ",\"logosWaiting\":" + (fs::exists(g_next, ec) ? "true" : "false") + "}";
}

std::string Dispatch(const std::string& aRequest)
{
    Json r;
    if (!ParseJson(aRequest, r))
        return Error("bad request");
    const std::string op = r.Str("op");
    try
    {
        if (op == "status") return Status();
        if (op == "list") return List();
        if (op == "preview")
            return Preview(r.Str("id"), r.Str("key"), r.Bool("invert"), static_cast<int>(r.Num("gap", 3)), static_cast<int>(r.Num("size", 96)));
        if (op == "search") return Search(r.Str("q"));
        if (op == "thumb") return Thumb(r.Str("url"), static_cast<int>(r.Num("size", 40)));
        if (op == "station") return Station(r.Str("id"), static_cast<int>(r.Num("gap", 3)));
        if (op == "styles") return Restyle(static_cast<int>(r.Num("gap", 3)));
        if (op == "install") return Install(r);
        if (op == "remove") return Remove(r.Str("id"));
        if (op == "removeAll") return RemoveAll();
    }
    catch (const std::exception& e)
    {
        Log(std::string("station manager error: ") + e.what());
        return Error(e.what());
    }
    return Error("unknown request " + op);
}

// background jobs
std::mutex g_jobsMutex;
std::map<int, std::pair<bool, std::string>> g_jobs;   // ticket -> done, answer
std::atomic<int> g_nextTicket{1};
} // namespace

void Init(const fs::path& aGameDir)
{
    g_game = aGameDir;
    const fs::path mods = aGameDir / L"bin\\x64\\plugins\\cyber_engine_tweaks\\mods";
    g_radios = mods / L"radioExt\\radios";
    g_data = mods / L"iHeartRadio\\data";
    g_logos = g_data / L"logos";
    g_archive = aGameDir / L"archive\\pc\\mod\\iHeartRadio_Stations.archive";
    g_next = g_data / L"iHeartRadio_Stations.archive.next";
    g_pendingFlag = g_data / L"logos_pending.txt";
    g_formatStamp = g_data / L"logos_format.txt";

    // logos saved while the game was running: the game hasn't opened the archive yet, so they can go in now
    std::error_code ec;
    std::lock_guard files(g_files);
    if (fs::exists(g_next, ec))
    {
        const bool empty = fs::file_size(g_next, ec) == 0;
        fs::create_directories(g_archive.parent_path(), ec);
        const bool ok = empty ? (!fs::exists(g_archive, ec) || DeleteFileW(g_archive.c_str()))
                              : MoveFileExW(g_next.c_str(), g_archive.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
        if (ok)
        {
            fs::remove(g_next, ec);
            fs::remove(g_pendingFlag, ec);
            WriteFile(g_formatStamp, kLogoFormat);
            Log("station logos from last time put in place");
        }
        else
            Log("could not put the waiting station logos in place (" + std::to_string(GetLastError()) + ")");
    }
    else if (IsFile(g_pendingFlag) || LogosOutdated())
    {
        std::string err;
        ApplyLogos(err);
        Log(err.empty() ? "station logos rebuilt" : "station logos: " + err);
    }
}

std::string Call(const std::string& aRequest) { return Dispatch(aRequest); }

int Begin(const std::string& aRequest)
{
    const int ticket = g_nextTicket++;
    {
        std::lock_guard lock(g_jobsMutex);
        g_jobs[ticket] = {false, std::string()};
    }
    std::thread([ticket, aRequest] {
        std::string answer;
        try
        {
            answer = Dispatch(aRequest);
        }
        catch (...)
        {
            answer = Error("unexpected error");
        }
        std::lock_guard lock(g_jobsMutex);
        g_jobs[ticket] = {true, std::move(answer)};
    }).detach();
    return ticket;
}

std::string Poll(int aTicket)
{
    std::lock_guard lock(g_jobsMutex);
    const auto it = g_jobs.find(aTicket);
    if (it == g_jobs.end())
        return Error("unknown ticket");
    if (!it->second.first)
        return {};
    std::string answer = std::move(it->second.second);
    g_jobs.erase(it);
    return answer;
}
} // namespace Stations
