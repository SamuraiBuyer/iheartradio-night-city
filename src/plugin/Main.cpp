// iHeart Radio - RED4ext plugin
//
// Runs the local MP3 bridge (Bridge.cpp) inside the game when it starts, so RadioExt can play iHeart stations from
// http://127.0.0.1:8925/zc<id>.mp3 without the player running anything by hand. No separate program: the bridge
// lives and ends with the game.

#include "Bridge.hpp"

#include <RED4ext/Common.hpp>
#include <RED4ext/Api/ApiVersion.hpp>
#include <RED4ext/Api/v1/EMainReason.hpp>
#include <RED4ext/Api/v1/PluginHandle.hpp>
#include <RED4ext/Api/v1/PluginInfo.hpp>
#include <RED4ext/Api/v1/Runtime.hpp>
#include <RED4ext/Api/v1/Sdk.hpp>
#include <RED4ext/Api/v1/Version.hpp>

#include <Windows.h>

#include <filesystem>
#include <string>

namespace
{
HMODULE g_module = nullptr;

std::filesystem::path PluginDir()
{
    wchar_t buf[MAX_PATH * 4]{};
    const DWORD len = GetModuleFileNameW(g_module, buf, static_cast<DWORD>(std::size(buf)));
    if (len == 0 || len >= std::size(buf))
        return {};
    return std::filesystem::path(buf).parent_path();
}

void StartBridge(const RED4ext::v1::Sdk* aSdk, RED4ext::v1::PluginHandle aHandle)
{
    const auto dir = PluginDir();
    std::string error = "plugin folder not found";
    try
    {
        if (!dir.empty() && Bridge::Start(dir, error))
        {
            aSdk->logger->Info(aHandle, "bridge running, its log: red4ext\\plugins\\iHeartRadio\\bridge.log");
            return;
        }
    }
    catch (...)
    {
        error = "unexpected error";
    }
    aSdk->logger->ErrorF(aHandle, "bridge not started: %s", error.c_str());
}
} // namespace

BOOL APIENTRY DllMain(HMODULE aModule, DWORD aReason, LPVOID)
{
    if (aReason == DLL_PROCESS_ATTACH)
    {
        g_module = aModule;
        DisableThreadLibraryCalls(aModule);
    }
    return TRUE;
}

RED4EXT_C_EXPORT bool RED4EXT_CALL Main(RED4ext::v1::PluginHandle aHandle, RED4ext::v1::EMainReason aReason,
                                        const RED4ext::v1::Sdk* aSdk)
{
    switch (aReason)
    {
    case RED4ext::v1::EMainReason::Load:
        StartBridge(aSdk, aHandle);
        break;
    case RED4ext::v1::EMainReason::Unload:
        Bridge::Stop();
        break;
    }
    return true;
}

RED4EXT_C_EXPORT void RED4EXT_CALL Query(RED4ext::v1::PluginInfo* aInfo)
{
    aInfo->name = L"iHeart Radio";
    aInfo->author = L"iHeart Radio mod";
    aInfo->version = RED4EXT_V1_SEMVER(2, 0, 4);
    aInfo->runtime = RED4EXT_V1_RUNTIME_VERSION_INDEPENDENT;
    aInfo->sdk = RED4EXT_V1_SDK_VERSION_CURRENT;
}

RED4EXT_C_EXPORT uint32_t RED4EXT_CALL Supports()
{
    return RED4EXT_API_VERSION_1;
}
