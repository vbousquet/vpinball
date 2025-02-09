// license:GPLv3+

#include "MsgPlugin.h"
#include "CorePlugin.h"

#include "altsound.h"

MsgPluginAPI* msgApi = nullptr;

uint32_t endpointId;

MSGPI_EXPORT void MSGPIAPI PluginLoad(const uint32_t sessionId, MsgPluginAPI* api)
{
   msgApi = api;
}

MSGPI_EXPORT void MSGPIAPI PluginUnload()
{
   msgApi = nullptr;
}
