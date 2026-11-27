// iHeart Radio - station logo .archive writer (see IconArchive.hpp). Same steps as IconArchive.cs.
//
// Format notes (checked against WolvenKit-built files):
//   archive (RDAR v12): file entries sorted by FNV-1a 64 of the depot path; index checksum CRC-64/XZ over the index
//     after the checksum field; SHA1 field unused (empty-string hash).
//   CR2W: table checksums CRC32; header checksum CRC32 of the 160-byte header with the field set to 0xDEADBEEF;
//     name hashes FNV-1a 64 folded to 32 bits; buffer checksum CRC32 of the stored buffer.
//   Textures are stored bottom row first.
#include "IconArchive.hpp"
#include "Resources.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstdio>
#include <cstring>

namespace IconArchive
{
namespace
{
// ------------------------------------------------------------------ checksums / hashes
uint32_t Crc32(const uint8_t* d, size_t n)
{
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; i++)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

uint64_t Crc64Xz(const uint8_t* d, size_t n)
{
    static const auto table = [] {
        std::array<uint64_t, 256> t{};
        for (uint64_t i = 0; i < 256; i++)
        {
            uint64_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xC96C5795D7870F42ull ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint64_t c = ~0ull;
    for (size_t i = 0; i < n; i++)
        c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c ^ ~0ull;
}

uint64_t Fnv1a64(const uint8_t* d, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++)
    {
        h ^= d[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

uint32_t NameHash(const uint8_t* d, size_t n)
{
    const uint64_t h = Fnv1a64(d, n);
    return static_cast<uint32_t>(h ^ (h >> 32));
}

// ------------------------------------------------------------------ little-endian access
using Bytes = std::vector<uint8_t>;

uint32_t U32(const Bytes& d, size_t o)
{
    uint32_t v;
    memcpy(&v, d.data() + o, 4);
    return v;
}
void Put(Bytes& d, size_t o, uint32_t v) { memcpy(d.data() + o, &v, 4); }
template <class T> void Append(Bytes& d, T v)
{
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    d.insert(d.end(), p, p + sizeof(T));
}
void Append(Bytes& d, const Bytes& b) { d.insert(d.end(), b.begin(), b.end()); }

size_t TableOff(const Bytes& d, int t) { return U32(d, 40 + 12 * t); }
size_t TableCount(const Bytes& d, int t) { return U32(d, 40 + 12 * t + 4); }
void TableCrc(Bytes& d, int t, size_t bytes) { Put(d, 40 + 12 * t + 8, Crc32(d.data() + TableOff(d, t), bytes)); }
void HeaderCrc(Bytes& d)
{
    Put(d, 32, 0xDEADBEEFu);
    Put(d, 32, Crc32(d.data(), 160));
}

// ------------------------------------------------------------------ templates (resources in the DLL)
bool Template(int aId, Bytes& aOut)
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&Template), &module);
    HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(aId), MAKEINTRESOURCEW(10));   // RT_RCDATA
    HGLOBAL data = res ? LoadResource(module, res) : nullptr;
    const void* p = data ? LockResource(data) : nullptr;
    if (!p)
        return false;
    const auto* b = static_cast<const uint8_t*>(p);
    aOut.assign(b, b + SizeofResource(module, res));
    return true;
}

bool PatchTexture(const Bytes& aTpl, const Bytes& aBlob, Bytes& d, std::string& aError)
{
    d = aTpl;
    const size_t b = TableOff(d, 5);   // buffers: flags, index, offset, diskSize, memSize, crc32
    if (U32(d, b + 16) != aBlob.size())
    {
        aError = "texture template size mismatch";
        return false;
    }
    Put(d, b + 8, static_cast<uint32_t>(d.size()));
    Put(d, b + 12, static_cast<uint32_t>(aBlob.size()));
    Put(d, b + 20, Crc32(aBlob.data(), aBlob.size()));
    TableCrc(d, 5, 24 * TableCount(d, 5));
    Put(d, 28, static_cast<uint32_t>(d.size() + aBlob.size()));   // buffersEnd
    HeaderCrc(d);
    return true;
}

bool PatchAtlas(const Bytes& aTpl, long long aStationId, Bytes& d, std::string& aError)
{
    d = aTpl;
    const size_t so = TableOff(d, 0), sc = TableCount(d, 0);
    const std::string from = "iheart_00000000", to = IconName(aStationId);
    int hits = 0;
    for (size_t i = so; i + from.size() <= so + sc; i++)
        if (memcmp(d.data() + i, from.data(), from.size()) == 0)
        {
            memcpy(d.data() + i, to.data(), to.size());
            hits++;
        }
    if (hits != 2)
    {
        aError = "atlas template: expected 2 names, found " + std::to_string(hits);
        return false;
    }
    TableCrc(d, 0, sc);
    const size_t no = TableOff(d, 1), nc = TableCount(d, 1);
    for (size_t k = 0; k < nc; k++)
    {
        const size_t off = so + U32(d, no + 8 * k);
        size_t end = off;
        while (end < d.size() && d[end] != 0)
            end++;
        Put(d, no + 8 * k + 4, end > off ? NameHash(d.data() + off, end - off) : 0);
    }
    TableCrc(d, 1, 8 * nc);
    HeaderCrc(d);
    return true;
}

// ------------------------------------------------------------------ BC7 (mode 6) for premultiplied white icons:
// every pixel is (v, v, v, v), so the colours of a block lie on one line and the end points are exact.
constexpr int kWeights[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

void WriteBlock(uint8_t* dst, int e0, int e1, const int* idx)
{
    uint64_t lo = 0, hi = 0;
    int pos = 0;
    auto put = [&](uint64_t val, int n) {
        val &= (1ull << n) - 1;
        if (pos < 64)
        {
            lo |= val << pos;
            if (pos + n > 64)
                hi |= val >> (64 - pos);
        }
        else
            hi |= val << (pos - 64);
        pos += n;
    };
    put(1ull << 6, 7);   // mode 6
    for (int c = 0; c < 4; c++)
    {
        put(static_cast<uint64_t>(e0 >> 1), 7);
        put(static_cast<uint64_t>(e1 >> 1), 7);
    }
    put(static_cast<uint64_t>(e0 & 1), 1);   // p-bits
    put(static_cast<uint64_t>(e1 & 1), 1);
    put(static_cast<uint64_t>(idx[0]), 3);
    for (int k = 1; k < 16; k++)
        put(static_cast<uint64_t>(idx[k]), 4);
    memcpy(dst, &lo, 8);
    memcpy(dst + 8, &hi, 8);
}

Bytes EncodeGrey(const Bytes& v, int width, int height)
{
    Bytes out(static_cast<size_t>(width) * height);
    int vals[16], pal[16], idx[16];
    size_t o = 0;
    for (int by = 0; by < height; by += 4)
        for (int bx = 0; bx < width; bx += 4)
        {
            int lo = 255, hi = 0;
            for (int k = 0; k < 16; k++)
            {
                const int p = v[(by + k / 4) * width + bx + k % 4];
                vals[k] = p;
                lo = std::min(lo, p);
                hi = std::max(hi, p);
            }
            for (int k = 0; k < 16; k++)
                pal[k] = ((64 - kWeights[k]) * lo + kWeights[k] * hi + 32) >> 6;
            for (int k = 0; k < 16; k++)
            {
                int best = 0, bd = INT_MAX;
                for (int j = 0; j < 16; j++)
                {
                    const int dd = abs(pal[j] - vals[k]);
                    if (dd < bd)
                    {
                        bd = dd;
                        best = j;
                    }
                }
                idx[k] = best;
            }
            int e0 = lo, e1 = hi;
            if (idx[0] >= 8)
            {
                e0 = hi;
                e1 = lo;
                for (int k = 0; k < 16; k++)
                    idx[k] = 15 - idx[k];
            }
            WriteBlock(out.data() + o, e0, e1, idx);
            o += 16;
        }
    return out;
}

// ------------------------------------------------------------------ RDAR archive
struct ArchiveFile
{
    std::string path;
    std::vector<Bytes> segments;
    std::vector<uint32_t> sizes;
    uint64_t hash = 0;
};

size_t Align(size_t p) { return (p + 4095) / 4096 * 4096; }

// padding to the 4 KB boundaries, with the byte WolvenKit-built archives use
void Pad(Bytes& d, size_t to) { d.resize(to, 0xD9); }

Bytes BuildArchive(std::vector<ArchiveFile> files)
{
    std::stable_sort(files.begin(), files.end(), [](const ArchiveFile& a, const ArchiveFile& b) { return a.hash < b.hash; });
    Bytes ms(0xAC, 0);

    // file name list (optional, lets WolvenKit and other tools show names)
    std::vector<const ArchiveFile*> byName;
    for (const auto& f : files)
        byName.push_back(&f);
    std::stable_sort(byName.begin(), byName.end(), [](const ArchiveFile* a, const ArchiveFile* b) { return a->path < b->path; });
    Bytes names;
    for (const auto* f : byName)
    {
        names.insert(names.end(), f->path.begin(), f->path.end());
        names.push_back(0);
    }
    Bytes lx = {'S', 'R', 'X', 'L'};
    for (const int v : {1, static_cast<int>(names.size()), static_cast<int>(names.size()), static_cast<int>(files.size())})
        Append(lx, v);
    Append(lx, names);
    Append(ms, lx);

    struct Seg
    {
        uint64_t offset;
        uint32_t zsize, size;
    };
    std::vector<Seg> segs;
    std::vector<std::pair<int, int>> entries;   // first segment, end segment
    for (const auto& f : files)
    {
        const int first = static_cast<int>(segs.size());
        for (size_t i = 0; i < f.segments.size(); i++)
        {
            segs.push_back({ms.size(), static_cast<uint32_t>(f.segments[i].size()), f.sizes[i]});
            Append(ms, f.segments[i]);
        }
        entries.emplace_back(first, static_cast<int>(segs.size()));
    }
    const size_t idxPos = Align(ms.size());
    Pad(ms, idxPos);

    Bytes body;
    Append(body, static_cast<int32_t>(files.size()));
    Append(body, static_cast<int32_t>(segs.size()));
    Append(body, static_cast<int32_t>(0));
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const int64_t ts = (static_cast<int64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    static const uint8_t emptySha1[20] = {0xda, 0x39, 0xa3, 0xee, 0x5e, 0x6b, 0x4b, 0x0d, 0x32, 0x55,
                                          0xbf, 0xef, 0x95, 0x60, 0x18, 0x90, 0xaf, 0xd8, 0x07, 0x09};
    for (size_t i = 0; i < files.size(); i++)
    {
        Append(body, files[i].hash);
        Append(body, ts);
        Append(body, static_cast<int32_t>(0));
        Append(body, static_cast<int32_t>(entries[i].first));
        Append(body, static_cast<int32_t>(entries[i].second));
        Append(body, static_cast<int32_t>(0));
        Append(body, static_cast<int32_t>(0));
        body.insert(body.end(), emptySha1, emptySha1 + 20);
    }
    for (const auto& s : segs)
    {
        Append(body, s.offset);
        Append(body, s.zsize);
        Append(body, s.size);
    }

    Append(ms, static_cast<int32_t>(8));
    Append(ms, static_cast<int32_t>(8 + body.size()));
    Append(ms, Crc64Xz(body.data(), body.size()));
    Append(ms, body);
    const uint32_t idxSize = static_cast<uint32_t>(16 + body.size());
    Pad(ms, Align(ms.size()));

    Bytes header;
    Append(header, 0x52414452u);   // "RDAR"
    Append(header, 12u);
    Append(header, static_cast<uint64_t>(idxPos));
    Append(header, idxSize);
    Append(header, static_cast<uint64_t>(0));
    Append(header, 0u);
    Append(header, static_cast<uint64_t>(ms.size()));
    Append(header, static_cast<uint32_t>(lx.size()));
    std::copy(header.begin(), header.end(), ms.begin());
    return ms;
}

ArchiveFile MakeFile(const std::string& aPath, std::vector<Bytes> aSegments)
{
    ArchiveFile f;
    f.path = aPath;
    for (const auto& s : aSegments)
        f.sizes.push_back(static_cast<uint32_t>(s.size()));
    f.segments = std::move(aSegments);
    f.hash = Fnv1a64(reinterpret_cast<const uint8_t*>(aPath.data()), aPath.size());
    return f;
}
} // namespace

std::string IconName(long long aStationId)
{
    char buf[32];
    sprintf_s(buf, "iheart_%08lld", aStationId);
    return buf;
}

std::string AtlasPath(long long aStationId) { return "iheartradio\\icons\\" + IconName(aStationId) + ".inkatlas"; }

bool Build(const std::vector<std::pair<long long, std::vector<uint8_t>>>& aIcons, std::string& aOut, std::string& aError)
{
    Bytes xbmMain, atlasTpl;
    if (!Template(IDR_XBM_TEMPLATE, xbmMain) || !Template(IDR_ATLAS_TEMPLATE, atlasTpl))
    {
        aError = "logo templates missing from the plugin";
        return false;
    }
    std::vector<ArchiveFile> files;
    for (const auto& [id, alpha] : aIcons)
    {
        if (alpha.size() != static_cast<size_t>(kIconSize) * kIconSize)
        {
            aError = "logo of station " + std::to_string(id) + " has the wrong size";
            return false;
        }
        // the game wants the bottom row first
        Bytes flipped(alpha.size());
        for (int y = 0; y < kIconSize; y++)
            memcpy(&flipped[static_cast<size_t>(kIconSize - 1 - y) * kIconSize], &alpha[static_cast<size_t>(y) * kIconSize], kIconSize);
        const Bytes raw = EncodeGrey(flipped, kIconSize, kIconSize);
        Bytes main, atlas;
        if (!PatchTexture(xbmMain, raw, main, aError) || !PatchAtlas(atlasTpl, id, atlas, aError))
            return false;
        files.push_back(MakeFile("iheartradio\\icons\\" + IconName(id) + ".xbm", {main, raw}));
        files.push_back(MakeFile(AtlasPath(id), {atlas}));
    }
    const Bytes data = BuildArchive(std::move(files));
    aOut.assign(data.begin(), data.end());
    return true;
}
} // namespace IconArchive
