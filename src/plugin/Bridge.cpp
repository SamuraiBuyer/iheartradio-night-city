// iHeart Radio - local stream bridge (see Bridge.hpp).
//
// The game's radio (RadioExt / FMOD) can't play iHeart's AAC streams, so for every station the game tunes to this
// bridge reads the stream itself, decodes it (AAC: Windows' own decoder, or the built-in fdk-aac under CrossOver /
// Wine and where Windows has none; MP3: minimp3) and hands the game Ogg Vorbis (libvorbis). Nothing else runs.
#include "Bridge.hpp"
#include "Net.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <objbase.h>
#include <winhttp.h>
#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <wmcodecdsp.h>
#include <wrl/client.h>

// third-party code built into the plugin (src/plugin/vendor), warnings are theirs
#pragma warning(push, 0)
#include <aacdecoder_lib.h>
#include <vorbis/vorbisenc.h>
#define MINIMP3_IMPLEMENTATION
#include <minimp3.h>
#pragma warning(pop)

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace
{
using namespace Net;

constexpr unsigned short kPort = 8925;
constexpr int kOutRate = 48000;   // decoded audio: 48 kHz 16-bit stereo

fs::path g_nowFile;
bool g_mf = false;                // Windows' AAC decoder (Media Foundation) available
bool g_wine = false;              // running under CrossOver / Wine
std::mutex g_stateMutex;
std::map<std::string, int> g_active;              // station id -> listeners
std::map<std::string, std::string> g_srcs;        // station id -> stream URL
std::map<std::string, std::string> g_lastTrack;   // station id -> last logged text (poller thread only)
std::atomic<bool> g_stop{false};
SOCKET g_listen = INVALID_SOCKET;

// ------------------------------------------------------------------ sending, finding a station's stream
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
    if (body.empty() || !ParseJson(body, root))
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

// ------------------------------------------------------------------ AAC frames (ADTS)
constexpr int kAacRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};

struct Adts
{
    int object = 0, rateIndex = 0, channels = 0;
    size_t header = 0, length = 0;
};

bool ParseAdts(const unsigned char* p, size_t n, Adts& a)
{
    if (n < 7 || p[0] != 0xFF || (p[1] & 0xF6) != 0xF0)
        return false;
    a.object = (p[2] >> 6) + 1;
    a.rateIndex = (p[2] >> 2) & 0xF;
    a.channels = ((p[2] & 1) << 2) | (p[3] >> 6);
    a.header = (p[1] & 1) ? 7 : 9;
    a.length = (static_cast<size_t>(p[3] & 3) << 11) | (static_cast<size_t>(p[4]) << 3) | (p[5] >> 5);
    return a.rateIndex < 13 && a.length > a.header;
}

// size of an ID3 tag at p (HLS segments start with one), 0 when there's none
size_t Id3Size(const unsigned char* p, size_t n)
{
    if (n < 10 || p[0] != 'I' || p[1] != 'D' || p[2] != '3')
        return 0;
    const size_t body = (static_cast<size_t>(p[6] & 0x7F) << 21) | ((p[7] & 0x7F) << 14) | ((p[8] & 0x7F) << 7) | (p[9] & 0x7F);
    return 10 + body + ((p[5] & 0x10) ? 10 : 0);
}

// cuts a byte stream into whole AAC frames, skipping ID3 tags and anything that isn't a frame
class AdtsReader
{
public:
    void Add(const std::string& aData) { buf.append(aData); }

    // the next frame, ADTS header included; false when more data is needed
    bool Next(Adts& aInfo, std::string& aFrame)
    {
        while (buf.size() - pos >= 10)
        {
            const auto* p = reinterpret_cast<const unsigned char*>(buf.data()) + pos;
            const size_t left = buf.size() - pos;
            if (const size_t tag = Id3Size(p, left))
            {
                if (tag > left)
                    break;
                pos += tag;
                continue;
            }
            if (!ParseAdts(p, left, aInfo))
            {
                ++pos;
                continue;
            }
            if (aInfo.length > left)
                break;
            aFrame.assign(buf, pos, aInfo.length);
            pos += aInfo.length;
            return true;
        }
        buf.erase(0, pos);
        pos = 0;
        return false;
    }

    size_t Buffered() const { return buf.size() - pos; }

private:
    std::string buf;
    size_t pos = 0;
};

// ------------------------------------------------------------------ any decoded audio -> 48 kHz 16-bit stereo
class Stereo48k
{
public:
    // interleaved 16-bit samples, any rate / channel count (first two channels; mono is doubled)
    void Convert(const int16_t* s, size_t aFrames, int aChannels, int aRate, std::vector<int16_t>& aOut)
    {
        if (aChannels < 1 || aRate < 1)
            return;
        const double step = static_cast<double>(aRate) / kOutRate;
        for (size_t i = 0; i < aFrames; ++i)
        {
            const int l = s[i * aChannels];
            const int r = aChannels > 1 ? s[i * aChannels + 1] : l;
            if (aRate == kOutRate)
            {
                aOut.push_back(static_cast<int16_t>(l));
                aOut.push_back(static_cast<int16_t>(r));
                continue;
            }
            while (t < 1.0)
            {
                aOut.push_back(static_cast<int16_t>(prevL + (l - prevL) * t));
                aOut.push_back(static_cast<int16_t>(prevR + (r - prevR) * t));
                t += step;
            }
            t -= 1.0;
            prevL = l;
            prevR = r;
        }
    }

private:
    double t = 0.0;
    int prevL = 0, prevR = 0;
};

// one AAC frame (with its ADTS header) in, 48 kHz stereo out
class AacBackend
{
public:
    virtual ~AacBackend() = default;
    virtual bool Decode(const std::string& aFrame, size_t aHeader, std::vector<int16_t>& aOut) = 0;
    virtual void Flush() = 0;
    std::string description;
};

// ------------------------------------------------------------------ AAC: the decoder that comes with Windows
bool CreateMfAacDecoder(ComPtr<IMFTransform>& aOut)
{
    if (SUCCEEDED(CoCreateInstance(CLSID_CMSAACDecMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&aOut))))
        return true;
    // any other AAC decoder the system has registered
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Audio, MFAudioFormat_AAC};
    IMFActivate** found = nullptr;
    UINT32 count = 0;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_AUDIO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
                         &in, nullptr, &found, &count)))
        return false;
    bool ok = false;
    for (UINT32 i = 0; i < count; ++i)
    {
        if (!ok)
            ok = SUCCEEDED(found[i]->ActivateObject(IID_PPV_ARGS(&aOut)));
        found[i]->Release();
    }
    CoTaskMemFree(found);
    return ok;
}

class MfAac : public AacBackend
{
public:
    bool Open(const Adts& a)
    {
        if (!CreateMfAacDecoder(mft))
            return false;
        const int rate = kAacRates[a.rateIndex];
        const int channels = a.channels ? a.channels : 2;
        int typeRate = rate, typeChannels = channels;

        // AudioSpecificConfig. iHeart sends HE-AAC: the ADTS header only describes the core (for example 24 kHz mono)
        // and the extra parts (SBR: double rate, PS: stereo) are found in the frames. Saying so up front (HE-AAC v2
        // for mono, v1 for stereo) makes the decoder output the full 48 kHz stereo from the start.
        unsigned char asc[4]{};
        UINT32 ascSize = 2;
        int ext = -1;
        for (int i = 0; i < 13; ++i)
            if (kAacRates[i] == rate * 2)
                ext = i;
        if (a.object == 2 && rate <= 24000 && ext >= 0 && channels <= 2)
        {
            const unsigned object = channels == 1 ? 29 : 5;
            const unsigned bits = (object << 20) | (a.rateIndex << 16) | (channels << 12) | (ext << 8) | (2u << 3);
            const unsigned v = bits << 7;   // 25 bits, left aligned
            asc[0] = static_cast<unsigned char>(v >> 24);
            asc[1] = static_cast<unsigned char>(v >> 16);
            asc[2] = static_cast<unsigned char>(v >> 8);
            asc[3] = static_cast<unsigned char>(v);
            ascSize = 4;
            typeRate = rate * 2;
            typeChannels = 2;
        }
        else
        {
            const unsigned v = (static_cast<unsigned>(a.object) << 11) | (a.rateIndex << 7) | (channels << 3);
            asc[0] = static_cast<unsigned char>(v >> 8);
            asc[1] = static_cast<unsigned char>(v);
        }

        // MF_MT_USER_DATA = the part of HEAACWAVEINFO after WAVEFORMATEX (raw frames, profile unknown) + the config
        unsigned char userData[12 + 4]{};
        userData[2] = 0xFE;
        memcpy(userData + 12, asc, ascSize);

        ComPtr<IMFMediaType> type;
        if (FAILED(MFCreateMediaType(&type)))
            return false;
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, typeRate);
        type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, typeChannels);
        type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
        type->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
        type->SetBlob(MF_MT_USER_DATA, userData, 12 + ascSize);
        if (FAILED(mft->SetInputType(0, type.Get(), 0)) || !PickOutput())
            return false;
        mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        description = "Windows AAC decoder, " + std::to_string(outRate) + " Hz " + std::to_string(outChannels) + " ch";
        return true;
    }

    bool Decode(const std::string& aFrame, size_t aHeader, std::vector<int16_t>& aOut) override
    {
        const DWORD size = static_cast<DWORD>(aFrame.size() - aHeader);   // raw frame, without the ADTS header
        ComPtr<IMFSample> sample;
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(MFCreateSample(&sample)) || FAILED(MFCreateMemoryBuffer(size, &buffer)))
            return false;
        BYTE* dst = nullptr;
        if (FAILED(buffer->Lock(&dst, nullptr, nullptr)))
            return false;
        memcpy(dst, aFrame.data() + aHeader, size);
        buffer->Unlock();
        buffer->SetCurrentLength(size);
        sample->AddBuffer(buffer.Get());
        HRESULT hr = mft->ProcessInput(0, sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING)
        {
            Drain(aOut);
            hr = mft->ProcessInput(0, sample.Get(), 0);
        }
        Drain(aOut);
        return SUCCEEDED(hr);
    }

    void Flush() override { mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0); }

private:
    ComPtr<IMFTransform> mft;
    UINT32 outRate = 0, outChannels = 0;
    Stereo48k stereo;

    // 16-bit PCM in whatever rate / channels the decoder offers
    bool PickOutput()
    {
        for (DWORD i = 0;; ++i)
        {
            ComPtr<IMFMediaType> type;
            if (FAILED(mft->GetOutputAvailableType(0, i, &type)))
                return false;
            GUID sub{};
            UINT32 bits = 0;
            type->GetGUID(MF_MT_SUBTYPE, &sub);
            type->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
            if (sub != MFAudioFormat_PCM || bits != 16)
                continue;
            if (FAILED(mft->SetOutputType(0, type.Get(), 0)))
                return false;
            type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &outRate);
            type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &outChannels);
            return outRate > 0 && outChannels > 0;
        }
    }

    void Drain(std::vector<int16_t>& aOut)
    {
        for (int guard = 0; guard < 64; ++guard)
        {
            MFT_OUTPUT_STREAM_INFO info{};
            mft->GetOutputStreamInfo(0, &info);
            ComPtr<IMFSample> sample;
            if (!(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES))
            {
                ComPtr<IMFMediaBuffer> buffer;
                if (FAILED(MFCreateSample(&sample)) || FAILED(MFCreateMemoryBuffer(info.cbSize ? info.cbSize : 65536, &buffer)))
                    return;
                sample->AddBuffer(buffer.Get());
            }
            MFT_OUTPUT_DATA_BUFFER out{};
            out.pSample = sample.Get();
            DWORD status = 0;
            const HRESULT hr = mft->ProcessOutput(0, 1, &out, &status);
            if (out.pEvents)
                out.pEvents->Release();
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
            {
                if (!PickOutput())
                    return;
                continue;
            }
            if (FAILED(hr))
                return;   // MF_E_TRANSFORM_NEED_MORE_INPUT: all out
            if (!sample)
                sample.Attach(out.pSample);   // the decoder made its own sample
            ComPtr<IMFMediaBuffer> data;
            if (FAILED(sample->ConvertToContiguousBuffer(&data)))
                return;
            BYTE* p = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(data->Lock(&p, nullptr, &len)))
            {
                stereo.Convert(reinterpret_cast<const int16_t*>(p), len / (2 * outChannels), outChannels, outRate, aOut);
                data->Unlock();
            }
        }
    }
};

// ------------------------------------------------------------------ AAC: the decoder built into the plugin (fdk-aac)
// Used where Windows' decoder isn't there or can't be trusted: CrossOver / Wine, Windows "N" editions.
class FdkAac : public AacBackend
{
public:
    ~FdkAac() override
    {
        if (dec)
            aacDecoder_Close(dec);
    }

    bool Open()
    {
        dec = aacDecoder_Open(TT_MP4_ADTS, 1);
        if (!dec)
            return false;
        aacDecoder_SetParam(dec, AAC_PCM_MAX_OUTPUT_CHANNELS, 2);
        description = "built-in AAC decoder (fdk-aac)";
        return true;
    }

    bool Decode(const std::string& aFrame, size_t, std::vector<int16_t>& aOut) override
    {
        UCHAR* in = reinterpret_cast<UCHAR*>(const_cast<char*>(aFrame.data()));
        UINT size = static_cast<UINT>(aFrame.size()), valid = size;
        if (aacDecoder_Fill(dec, &in, &size, &valid) != AAC_DEC_OK)
            return false;
        bool ok = true;
        while (true)
        {
            const AAC_DECODER_ERROR err = aacDecoder_DecodeFrame(dec, pcm.data(), static_cast<INT>(pcm.size()), 0);
            if (err == AAC_DEC_NOT_ENOUGH_BITS)
                break;
            if (err != AAC_DEC_OK)
            {
                ok = false;
                break;
            }
            const CStreamInfo* info = aacDecoder_GetStreamInfo(dec);
            if (!info || info->numChannels < 1 || info->frameSize < 1)
                break;
            stereo.Convert(pcm.data(), static_cast<size_t>(info->frameSize), info->numChannels, info->sampleRate, aOut);
        }
        return ok;
    }

    void Flush() override { aacDecoder_SetParam(dec, AAC_TPDEC_CLEAR_BUFFER, 1); }

private:
    HANDLE_AACDECODER dec = nullptr;
    std::vector<INT_PCM> pcm = std::vector<INT_PCM>(2048 * 8);
    Stereo48k stereo;
};

// ------------------------------------------------------------------ a station's stream -> 48 kHz stereo
class Decoder
{
public:
    virtual ~Decoder() = default;
    virtual void Add(const std::string& aData) = 0;
    // decodes what has come in so far (appends to aOut); false when the stream can't be decoded
    virtual bool Pull(std::vector<int16_t>& aOut) = 0;
    // after reconnecting: forget the half frame of the old connection
    virtual void Reset() = 0;
    std::string description;
};

class AacStream : public Decoder
{
public:
    void Add(const std::string& aData) override { reader.Add(aData); }

    bool Pull(std::vector<int16_t>& aOut) override
    {
        Adts info;
        while (reader.Next(info, frame))
        {
            if (!backend && !Start(info))
                return false;
            if (backend->Decode(frame, info.header, aOut))
                errors = 0;
            else if (++errors > 200)
                return false;
        }
        return reader.Buffered() < (256u << 10);   // that much without a single frame: not AAC after all
    }

    void Reset() override
    {
        reader = AdtsReader();
        if (backend)
            backend->Flush();
    }

private:
    AdtsReader reader;
    std::string frame;
    std::unique_ptr<AacBackend> backend;
    int errors = 0;

    bool Start(const Adts& aInfo)
    {
        if (g_mf)
        {
            auto mf = std::make_unique<MfAac>();
            if (mf->Open(aInfo))
                backend = std::move(mf);
        }
        if (!backend)
        {
            auto fdk = std::make_unique<FdkAac>();
            if (fdk->Open())
                backend = std::move(fdk);
        }
        if (!backend)
            return false;
        description = "AAC " + std::to_string(kAacRates[aInfo.rateIndex]) + " Hz, " + backend->description;
        return true;
    }
};

class Mp3Stream : public Decoder
{
public:
    Mp3Stream()
    {
        mp3dec_init(&dec);
        description = "MP3 (minimp3)";
    }

    void Add(const std::string& aData) override { buf.append(aData); }

    bool Pull(std::vector<int16_t>& aOut) override
    {
        // minimp3 wants a few frames to look at: decode while there's at least 16 KB, keep the rest for later
        while (buf.size() - pos >= 16384)
        {
            mp3dec_frame_info_t info{};
            const int samples = mp3dec_decode_frame(&dec, reinterpret_cast<const uint8_t*>(buf.data()) + pos,
                                                    static_cast<int>(buf.size() - pos), pcm.data(), &info);
            if (info.frame_bytes == 0)
            {
                // nothing that looks like MP3 in there
                pos = buf.size();
                return ++misses < 16;
            }
            misses = 0;
            pos += info.frame_bytes;
            if (samples > 0)
                stereo.Convert(pcm.data(), static_cast<size_t>(samples), info.channels, info.hz, aOut);
        }
        buf.erase(0, pos);
        pos = 0;
        return true;
    }

    void Reset() override
    {
        buf.clear();
        pos = 0;
        mp3dec_init(&dec);
    }

private:
    mp3dec_t dec{};
    std::string buf;
    size_t pos = 0;
    int misses = 0;
    std::vector<mp3d_sample_t> pcm = std::vector<mp3d_sample_t>(MINIMP3_MAX_SAMPLES_PER_FRAME);
    Stereo48k stereo;
};

// ------------------------------------------------------------------ what the game gets
// Ogg Vorbis for station URLs ending in .ogg (FMOD plays a live Ogg stream for as long as it lasts), WAV for the
// older .mp3 URLs (FMOD needs a WAV size, and plays 2 GB and more as silence: 2 GB - 1 is about 3 hours).
class Output
{
public:
    explicit Output(bool aOgg) : ogg(aOgg) {}

    ~Output()
    {
        if (!vorbisReady)
            return;
        ogg_stream_clear(&os);
        vorbis_block_clear(&vb);
        vorbis_dsp_clear(&vd);
        vorbis_comment_clear(&vc);
        vorbis_info_clear(&vi);
    }

    // HTTP answer + the format's own header
    bool Begin(SOCKET aClient, const std::string& aName)
    {
        std::string safeName;
        for (char ch : aName)
            if (ch >= 0x20 && ch <= 0x7E)
                safeName += ch;
        std::string out = "HTTP/1.0 200 OK\r\nContent-Type: " + std::string(ogg ? "audio/ogg" : "audio/wav") +
                          "\r\nicy-name: " + safeName + "\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n";
        if (ogg)
        {
            vorbis_info_init(&vi);
            if (vorbis_encode_init_vbr(&vi, 2, kOutRate, 0.6f) != 0)   // about 190 kbit/s
            {
                vorbis_info_clear(&vi);
                return false;
            }
            vorbis_comment_init(&vc);
            vorbis_analysis_init(&vd, &vi);
            vorbis_block_init(&vd, &vb);
            ogg_stream_init(&os, 1);
            vorbisReady = true;
            ogg_packet id, comment, books;
            vorbis_analysis_headerout(&vd, &vc, &id, &comment, &books);
            ogg_stream_packetin(&os, &id);
            ogg_stream_packetin(&os, &comment);
            ogg_stream_packetin(&os, &books);
            ogg_page page;
            while (ogg_stream_flush(&os, &page))
                Append(out, page);
        }
        else
            out += WavHeader();
        return SendText(aClient, out);
    }

    // 48 kHz stereo samples; false when the game stopped listening
    bool Write(SOCKET aClient, const std::vector<int16_t>& aPcm)
    {
        if (aPcm.empty())
            return true;
        if (!ogg)
            return SendAll(aClient, reinterpret_cast<const char*>(aPcm.data()), static_cast<int>(aPcm.size() * 2));
        const int frames = static_cast<int>(aPcm.size() / 2);
        float** in = vorbis_analysis_buffer(&vd, frames);
        for (int i = 0; i < frames; ++i)
        {
            in[0][i] = aPcm[2 * i] / 32768.0f;
            in[1][i] = aPcm[2 * i + 1] / 32768.0f;
        }
        vorbis_analysis_wrote(&vd, frames);
        std::string out;
        while (vorbis_analysis_blockout(&vd, &vb) == 1)
        {
            vorbis_analysis(&vb, nullptr);
            vorbis_bitrate_addblock(&vb);
            ogg_packet packet;
            while (vorbis_bitrate_flushpacket(&vd, &packet))
            {
                ogg_stream_packetin(&os, &packet);
                ogg_page page;
                while (ogg_stream_pageout(&os, &page))
                    Append(out, page);
            }
        }
        return out.empty() || SendText(aClient, out);
    }

private:
    bool ogg;
    bool vorbisReady = false;
    vorbis_info vi{};
    vorbis_comment vc{};
    vorbis_dsp_state vd{};
    vorbis_block vb{};
    ogg_stream_state os{};

    static void Append(std::string& aOut, const ogg_page& aPage)
    {
        aOut.append(reinterpret_cast<const char*>(aPage.header), aPage.header_len);
        aOut.append(reinterpret_cast<const char*>(aPage.body), aPage.body_len);
    }

    static std::string WavHeader()
    {
        std::string h;
        auto put = [&h](uint32_t v, int n) {
            for (int i = 0; i < n; ++i)
                h += static_cast<char>((v >> (8 * i)) & 0xFF);
        };
        const uint32_t data = 0x7FFFFFFFu - 36;
        h += "RIFF";
        put(data + 36, 4);
        h += "WAVEfmt ";
        put(16, 4);
        put(1, 2);
        put(2, 2);
        put(kOutRate, 4);
        put(kOutRate * 4, 4);
        put(4, 2);
        put(16, 2);
        h += "data";
        put(data, 4);
        return h;
    }
};

// ------------------------------------------------------------------ reading a station: one stream, or HLS segments
std::string ResolveUrl(const std::string& aBase, const std::string& aRef)
{
    if (aRef.rfind("http://", 0) == 0 || aRef.rfind("https://", 0) == 0)
        return aRef;
    const size_t scheme = aBase.find("://");
    if (scheme == std::string::npos)
        return aRef;
    if (aRef.rfind("//", 0) == 0)
        return aBase.substr(0, scheme + 1) + aRef;
    const size_t hostEnd = aBase.find('/', scheme + 3);
    if (hostEnd == std::string::npos)
        return aBase + (aRef.empty() || aRef[0] != '/' ? "/" : "") + aRef;
    if (!aRef.empty() && aRef[0] == '/')
        return aBase.substr(0, hostEnd) + aRef;
    const std::string path = aBase.substr(0, aBase.find('?'));
    return path.substr(0, path.rfind('/') + 1) + aRef;
}

class Upstream
{
public:
    std::string type;   // Content-Type, lower case

    bool Open(const std::string& aUrl)
    {
        type.clear();
        http = std::make_unique<Http>();
        hls = Lower(aUrl).find(".m3u8") != std::string::npos;
        queue.clear();
        nextSeq = -1;
        failures = 0;
        if (!hls)
        {
            if (!::Open(*http, aUrl, 15000) || http->status != 200)
                return false;
            type = ContentType(*http);
            return true;
        }
        // master playlist -> the first variant (iHeart has one)
        std::string final;
        const std::string master = Get(aUrl, 10000, &final);
        if (master.empty())
            return false;
        playlist = final;
        if (master.find("#EXT-X-STREAM-INF") != std::string::npos)
            for (const auto& line : Lines(master))
                if (line[0] != '#')
                {
                    playlist = ResolveUrl(final, line);
                    break;
                }
        return Load();
    }

    // the next piece of the stream; false when it ended
    bool Read(std::string& aChunk)
    {
        aChunk.clear();
        if (!hls)
        {
            char buf[16384];
            DWORD n = 0;
            if (!WinHttpReadData(http->request, buf, sizeof(buf), &n) || n == 0)
                return false;
            aChunk.assign(buf, n);
            return true;
        }
        while (!g_stop)
        {
            if (!queue.empty())
            {
                const std::string url = queue.front();
                queue.pop_front();
                std::string segType;
                aChunk = Get(url, 15000, nullptr, &segType);
                if (!aChunk.empty())
                {
                    if (type.empty())
                        type = segType;
                    failures = 0;
                    return true;
                }
                if (++failures > 5)
                    return false;
                continue;
            }
            // new segments show up every few seconds
            if (GetTickCount64() - loaded >= static_cast<ULONGLONG>(target) * 500)
            {
                if (!Load() && ++failures > 5)
                    return false;
            }
            else
                Sleep(250);
        }
        return false;
    }

private:
    std::unique_ptr<Http> http;
    bool hls = false;
    std::string playlist;
    std::deque<std::string> queue;
    long long nextSeq = -1;
    int target = 10;
    ULONGLONG loaded = 0;
    int failures = 0;

    static std::vector<std::string> Lines(const std::string& aText)
    {
        std::vector<std::string> out;
        size_t start = 0;
        while (start < aText.size())
        {
            size_t nl = aText.find('\n', start);
            if (nl == std::string::npos)
                nl = aText.size();
            const std::string line = Trim(aText.substr(start, nl - start));
            if (!line.empty())
                out.push_back(line);
            start = nl + 1;
        }
        return out;
    }

    bool Load()
    {
        loaded = GetTickCount64();
        std::string final;
        const std::string body = Get(playlist, 10000, &final);
        if (body.empty())
            return false;
        long long seq = 0;
        std::vector<std::pair<long long, std::string>> segments;
        for (const auto& line : Lines(body))
        {
            if (line.rfind("#EXT-X-TARGETDURATION:", 0) == 0)
                target = (std::max)(1, atoi(line.c_str() + 22));
            else if (line.rfind("#EXT-X-MEDIA-SEQUENCE:", 0) == 0)
                seq = atoll(line.c_str() + 22);
            else if (line[0] != '#')
                segments.emplace_back(seq++, ResolveUrl(final, line));
        }
        if (segments.empty())
            return false;
        // start close to live; after that, only the new ones (and start over if the numbering was reset)
        if (nextSeq < 0 || segments.back().first + 1 < nextSeq - 50)
            nextSeq = segments[segments.size() > 3 ? segments.size() - 3 : 0].first;
        for (const auto& [n, url] : segments)
            if (n >= nextSeq)
            {
                queue.push_back(url);
                nextSeq = n + 1;
            }
        return true;
    }
};

// ------------------------------------------------------------------ serving one station to the game
// while the game plays a station, the now-playing poller follows it
struct Listening
{
    std::string id;
    Listening(const std::string& aId, const std::string& aSrc) : id(aId)
    {
        std::lock_guard lock(g_stateMutex);
        ++g_active[id];
        g_srcs[id] = aSrc;
    }
    ~Listening()
    {
        std::lock_guard lock(g_stateMutex);
        if (--g_active[id] <= 0)
            g_active.erase(id);
    }
};

enum class Kind { Mp3, Aac, Other };

Kind Sniff(const std::string& aType, const std::string& aData)
{
    if (aType.find("mpeg") != std::string::npos || aType.find("mp3") != std::string::npos)
        return Kind::Mp3;
    if (aType.find("aac") != std::string::npos || aType.find("mp4a") != std::string::npos)
        return Kind::Aac;
    const auto* p = reinterpret_cast<const unsigned char*>(aData.data());
    for (size_t i = Id3Size(p, aData.size()); i + 1 < aData.size(); ++i)
        if (p[i] == 0xFF && (p[i + 1] & 0xE0) == 0xE0)
            return (p[i + 1] & 0x06) == 0 ? Kind::Aac : Kind::Mp3;   // layer bits 0 = ADTS
    return Kind::Other;
}

// the station's stream dropped: try again for a while. aKeepAlive (may be empty) keeps the game fed meanwhile and
// returns false when the game stopped listening.
bool Reconnect(Upstream& aUp, const std::string& aSrc, const std::string& aName, const std::function<bool()>& aKeepAlive)
{
    Log(aName + ": stream dropped, reconnecting");
    for (int attempt = 0; attempt < 15 && !g_stop; ++attempt)
    {
        if (aKeepAlive && !aKeepAlive())
            return false;
        Sleep(1000);
        if (aUp.Open(aSrc))
        {
            Log(aName + ": reconnected");
            return true;
        }
    }
    Log(aName + ": could not reconnect");
    return false;
}

// MP3 station asked for as .mp3: FMOD plays it as it is
void PassThrough(SOCKET aClient, Upstream& aUp, std::string aChunk, const std::string& aSrc, const std::string& aName)
{
    Log(aName + ": MP3, passed through");
    if (!SendText(aClient, "HTTP/1.0 200 OK\r\nContent-Type: audio/mpeg\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n"))
        return;
    while (!g_stop)
    {
        if (!aChunk.empty() && !SendAll(aClient, aChunk.data(), static_cast<int>(aChunk.size())))
            return;   // the game switched away
        if (!aUp.Read(aChunk) && !Reconnect(aUp, aSrc, aName, {}))
            return;
    }
}

// Decodes the station and sends it as Ogg Vorbis / WAV. False (nothing sent yet) when it can't be decoded.
bool Convert(SOCKET aClient, Upstream& aUp, std::string aChunk, Decoder& aDecoder, bool aOgg, const std::string& aSrc,
             const std::string& aName)
{
    // decode the first bit before answering, so a stream that can't be played gets a clean error
    std::vector<int16_t> pcm;
    aDecoder.Add(aChunk);
    size_t read = aChunk.size();
    while (pcm.empty())
    {
        if (!aDecoder.Pull(pcm) || read > (512u << 10))
        {
            Log(aName + ": couldn't decode the stream (" + (aUp.type.empty() ? "unknown format" : aUp.type) + ")");
            return false;
        }
        if (!pcm.empty())
            break;
        if (!aUp.Read(aChunk))
        {
            Log(aName + ": the stream ended right away");
            return false;
        }
        read += aChunk.size();
        aDecoder.Add(aChunk);
    }
    Log(aName + ": " + aDecoder.description + " -> " + (aOgg ? "Ogg Vorbis" : "WAV"));

    Output out(aOgg);
    if (!out.Begin(aClient, aName) || !out.Write(aClient, pcm))
        return true;
    const std::vector<int16_t> quiet(kOutRate * 2, 0);   // one second
    while (!g_stop)
    {
        if (!aUp.Read(aChunk))
        {
            if (!Reconnect(aUp, aSrc, aName, [&] { return out.Write(aClient, quiet); }))
                return true;
            aDecoder.Reset();
            continue;
        }
        aDecoder.Add(aChunk);
        pcm.clear();
        if (!aDecoder.Pull(pcm))
        {
            Log(aName + ": the stream can't be decoded any more, stopping");
            return true;
        }
        if (!out.Write(aClient, pcm))
            return true;   // the game switched away
    }
    return true;
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
    static const std::regex re(R"(^(GET|HEAD) /zc(\d+)(\.mp3|\.ogg)?[ ?])");
    std::smatch m;
    if (!std::regex_search(request, m, re))
    {
        SendText(aClient, "HTTP/1.0 404 Not Found\r\nConnection: close\r\n\r\nUse /zc<station id>.ogg\r\n");
        return;
    }
    const std::string method = m[1].str(), id = m[2].str();
    const bool ogg = m[3].str() == ".ogg";

    std::string name, src;
    Resolve(id, name, src);
    Log(method + " " + name + "  (" + src + ")");
    if (method == "HEAD")
    {
        SendText(aClient, std::string("HTTP/1.0 200 OK\r\nContent-Type: ") + (ogg ? "audio/ogg" : "audio/wav") +
                              "\r\nConnection: close\r\n\r\n");
        return;
    }

    Listening listening(id, src);
    Upstream up;
    std::string first;
    if (!up.Open(src) || !up.Read(first))
    {
        Log(name + ": could not open the stream");
        SendText(aClient, "HTTP/1.0 503 Service Unavailable\r\nConnection: close\r\n\r\n");
        return;
    }
    const Kind kind = Sniff(up.type, first);
    if (kind == Kind::Mp3 && !ogg)
        return PassThrough(aClient, up, first, src, name);

    std::unique_ptr<Decoder> decoder;
    if (kind == Kind::Mp3)
        decoder = std::make_unique<Mp3Stream>();
    else
        decoder = std::make_unique<AacStream>();   // AAC, or anything unknown: give AAC a try
    if (!Convert(aClient, up, first, *decoder, ogg, src, name))
        SendText(aClient, "HTTP/1.0 503 Service Unavailable\r\nConnection: close\r\n\r\n");
}

// Windows' AAC decoder: there on normal Windows. Not used under CrossOver / Wine (the built-in one is, so it sounds
// the same everywhere) or where Media Foundation is missing (mfplat.dll is delay-loaded for that: Windows "N"
// editions without the Media Feature Pack).
void FindDecoder()
{
    if (g_wine)
    {
        Log("AAC decoder: built-in (fdk-aac)");
        return;
    }
    if (LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) && SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
    {
        ComPtr<IMFTransform> probe;
        g_mf = CreateMfAacDecoder(probe);
    }
    Log(g_mf ? "AAC decoder: Windows (Media Foundation), built-in fdk-aac as backup" : "AAC decoder: built-in (fdk-aac)");
}

void Serve()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    FindDecoder();
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
            const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            try
            {
                Handle(client);
            }
            catch (...)
            {
                Log("error while serving a station");
            }
            closesocket(client);
            if (SUCCEEDED(com))
                CoUninitialize();
        }).detach();
    }
}

// Stations made by older versions of the Station Manager point at .../zc<id>.mp3, which the bridge answers with WAV
// (about 3 hours at a time). Point them at .ogg instead. RadioExt reads these files after this plugin has started.
void UpgradeStations(const fs::path& aRadios)
{
    std::error_code ec;
    static const std::regex oldUrl(R"((127\.0\.0\.1:8925/zc\d+)\.mp3)");
    int changed = 0;
    for (const auto& dir : fs::directory_iterator(aRadios, ec))
    {
        const fs::path file = dir.path() / L"metadata.json";
        if (!dir.is_directory(ec) || dir.path().filename().wstring().rfind(L"iheart_", 0) != 0 || !fs::is_regular_file(file, ec))
            continue;
        std::string text;
        {
            std::ifstream f(file, std::ios::binary);
            text.assign(std::istreambuf_iterator<char>(f), {});
        }
        const std::string updated = std::regex_replace(text, oldUrl, "$1.ogg");
        if (updated == text)
            continue;
        const fs::path tmp = file.wstring() + L".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f << updated;
        }
        if (MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING))
            ++changed;
    }
    if (changed)
        Log("updated " + std::to_string(changed) + " station(s) to the .ogg stream address");
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
    if (!body.empty() && ParseJson(body, j))   // 204 / 410 (nothing on air right now) give an empty body
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
    SetLog(aPluginDir / L"bridge.log");

    // CrossOver / Wine: worth knowing when reading a log
    using WineVersion = const char*(__cdecl*)();
    if (const auto wine = reinterpret_cast<WineVersion>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")))
    {
        g_wine = true;
        Log(std::string("running under Wine ") + wine());
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
        UpgradeStations(cetMods / L"radioExt\\radios");
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

    Log("iHeart Radio bridge running on http://127.0.0.1:" + std::to_string(kPort) + "/zc<id>.ogg");
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
}
} // namespace Bridge
