// iHeart Radio - the functions the CET menu calls (registered with the game's script system, like RadioExt does):
//   IHeartRadio.Call(request)  -> answer       quick requests
//   IHeartRadio.Begin(request) -> ticket       slow ones (internet, logos), run in the background
//   IHeartRadio.Poll(ticket)   -> answer, or "" while it's still running
// Requests and answers are JSON, see Stations.hpp.
#include "Script.hpp"
#include "Stations.hpp"

#pragma warning(push, 0)
#include <RED4ext/RED4ext.hpp>
#include <RED4ext/RTTITypes.hpp>
#include <RED4ext/Scripting/IScriptable.hpp>
#pragma warning(pop)

#include <string>

namespace
{
struct IHeartRadioScript : RED4ext::IScriptable
{
    RED4ext::CClass* GetNativeType();
};

RED4ext::TTypedClass<IHeartRadioScript> g_class("IHeartRadio");

RED4ext::CClass* IHeartRadioScript::GetNativeType() { return &g_class; }

void ReturnString(RED4ext::CString* aOut, const std::string& aValue)
{
    if (!aOut)
        return;
    RED4ext::CString value(aValue.c_str());
    RED4ext::CRTTISystem::Get()->GetType("String")->Assign(aOut, &value);
}

void Call(RED4ext::IScriptable*, RED4ext::CStackFrame* aFrame, RED4ext::CString* aOut, int64_t)
{
    RED4ext::CString request;
    RED4ext::GetParameter(aFrame, &request);
    aFrame->code++;   // skip ParamEnd
    ReturnString(aOut, Stations::Call(request.c_str()));
}

void Begin(RED4ext::IScriptable*, RED4ext::CStackFrame* aFrame, int32_t* aOut, int64_t)
{
    RED4ext::CString request;
    RED4ext::GetParameter(aFrame, &request);
    aFrame->code++;
    const int32_t ticket = Stations::Begin(request.c_str());
    if (aOut)
        RED4ext::CRTTISystem::Get()->GetType("Int32")->Assign(aOut, &ticket);
}

void Poll(RED4ext::IScriptable*, RED4ext::CStackFrame* aFrame, RED4ext::CString* aOut, int64_t)
{
    int32_t ticket = 0;
    RED4ext::GetParameter(aFrame, &ticket);
    aFrame->code++;
    ReturnString(aOut, Stations::Poll(ticket));
}

void RegisterTypes()
{
    RED4ext::CNamePool::Add("IHeartRadio");
    g_class.flags = {.isNative = true};
    RED4ext::CRTTISystem::Get()->RegisterType(&g_class);
}

void PostRegisterTypes()
{
    auto rtti = RED4ext::CRTTISystem::Get();
    g_class.parent = rtti->GetClass("IScriptable");

    auto call = RED4ext::CClassStaticFunction::Create(&g_class, "Call", "Call", &Call, {.isNative = true, .isStatic = true});
    call->AddParam("String", "request");
    call->SetReturnType("String");

    auto begin = RED4ext::CClassStaticFunction::Create(&g_class, "Begin", "Begin", &Begin, {.isNative = true, .isStatic = true});
    begin->AddParam("String", "request");
    begin->SetReturnType("Int32");

    auto poll = RED4ext::CClassStaticFunction::Create(&g_class, "Poll", "Poll", &Poll, {.isNative = true, .isStatic = true});
    poll->AddParam("Int32", "ticket");
    poll->SetReturnType("String");

    g_class.RegisterFunction(call);
    g_class.RegisterFunction(begin);
    g_class.RegisterFunction(poll);
}
} // namespace

namespace Script
{
void Register()
{
    RED4ext::CRTTISystem::Get()->AddRegisterCallback(RegisterTypes);
    RED4ext::CRTTISystem::Get()->AddPostRegisterCallback(PostRegisterTypes);
}
} // namespace Script
