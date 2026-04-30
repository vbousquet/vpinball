// license:GPLv3+

#include "common.h"
#include "plugins/MsgPlugin.h"
#include "plugins/LoggingPlugin.h"
#include "plugins/ScriptablePlugin.h"
#include "plugins/ControllerPlugin.h"
#include "plugins/VPXPlugin.h"

#include <filesystem>
#include <cassert>
#include <charconv>

namespace Pin2K {

// Scriptable object definitions

class Controller
{
public:
   PSC_IMPLEMENT_REFCOUNT()
};

// For now, we only declare a minimal Controller scriptable interface
PSC_CLASS_START(Pin2K_Controller, Controller)
PSC_CLASS_END()

// Plugin interface

static const MsgPluginAPI* msgApi = nullptr;
static ScriptablePluginAPI* scriptApi = nullptr;
static unsigned int getScriptApiMsgId = 0;
static unsigned int getVpxApiMsgId = 0;

static uint32_t endpointId;

PSC_ERROR_IMPLEMENT(scriptApi); // Implement script error

LPI_IMPLEMENT_CPP // Implement shared log support

}

// Plugin lifecycle

using namespace Pin2K;

MSGPI_EXPORT void MSGPIAPI Pin2KPluginLoad(const uint32_t sessionId, const MsgPluginAPI* api)
{
   endpointId = sessionId;
   msgApi = api;

   // Optional VPX API
   getVpxApiMsgId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_MSG_GET_API);

   // Request and setup shared login API
   LPISetup(endpointId, msgApi);

   // Contribute our API to the script engine
   getScriptApiMsgId = msgApi->GetMsgID(SCRIPTPI_NAMESPACE, SCRIPTPI_MSG_GET_API);
   msgApi->BroadcastMsg(endpointId, getScriptApiMsgId, &scriptApi);

   auto regLambda = [](ScriptClassDef* scd) { scriptApi->RegisterScriptClass(scd); };
   RegisterPin2K_Controller(regLambda);

   Pin2K_Controller_SCD->CreateObject = []()
   {
      // TODO: Instantiate and return the Encore controller wrapper
      return static_cast<void*>(nullptr);
   };
   scriptApi->SubmitTypeLibrary(endpointId);
   scriptApi->SetCOMObjectOverride("VPin2K.Controller", Pin2K_Controller_SCD);

   LOGI("Pin2K Plugin loaded.");
}

MSGPI_EXPORT void MSGPIAPI Pin2KPluginUnload()
{
   scriptApi->SetCOMObjectOverride("VPin2K.Controller", nullptr);
   auto regLambda = [](ScriptClassDef* scd) { scriptApi->UnregisterScriptClass(scd); };
   UnregisterPin2K_Controller(regLambda);

   msgApi->ReleaseMsgID(getVpxApiMsgId);
   msgApi->ReleaseMsgID(getScriptApiMsgId);
   msgApi->FlushPendingCallbacks(endpointId);
   msgApi = nullptr;
}
