// license:GPLv3+

// This file implements a backglass renderer for the B2S file and data format
// It is implemented as a plugin as it is expected to be a plugin but as it
// uses VPX's internal renderer which is not yet opened to plugins, it must be
// compiled inside VPX and directly registered to the plugin system.

#include "core/stdafx.h"

#include "common.h"

#include "Server.h"

static const MsgPluginAPI* msgApi = nullptr;
static VPXPluginAPI* vpxApi = nullptr;

static uint32_t endpointId;
static unsigned int onGameStartId, onGameEndId, onPrepareFrameId;


///////////////////////////////////////////////////////////////////////////////////////////////////
// Scripting API
//
// This API is used for original tables directly creating and animating the backglass from script
// It is also used for legacy tables, not using the common script, that create the backglass 
// object, expecting it to forward call to PinMame.
//

// TODO implement scripting


///////////////////////////////////////////////////////////////////////////////////////////////////
// Renderer implementation

static B2S::Server* server = nullptr;

void onPrepareFrame(const unsigned int eventId, void* userData, void* eventData)
{
   assert(server != nullptr);
   server->Render();
}

#include "forms/FormBackglass.h"

void onGameStart(const unsigned int eventId, void* userData, void* eventData)
{
   assert(server == nullptr);
   msgApi->SubscribeMsg(endpointId, onPrepareFrameId, onPrepareFrame, nullptr);

   VPXTableInfo info;
   vpxApi->GetTableInfo(&info);
   B2S::B2SData* data = B2S::B2SData::GetInstance();
   data->SetTableFileName(info.path);

   server = new B2S::Server();
   server->Start();
}

void onGameEnd(const unsigned int eventId, void* userData, void* eventData)
{
   assert(server != nullptr);
   server->Release();
   msgApi->UnsubscribeMsg(onPrepareFrameId, onPrepareFrame);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
//

MSGPI_EXPORT void MSGPIAPI PluginLoad(const uint32_t sessionId, const MsgPluginAPI* api)
{
   msgApi = api;
   endpointId = sessionId;
   unsigned int getVpxApiId;
   msgApi->BroadcastMsg(sessionId, getVpxApiId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_MSG_GET_API), &vpxApi);
   msgApi->ReleaseMsgID(getVpxApiId);
   if (vpxApi == nullptr)
      return;
   
   onPrepareFrameId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_PREPARE_FRAME);
   msgApi->SubscribeMsg(endpointId, onGameStartId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_GAME_START), onGameStart, nullptr);
   msgApi->SubscribeMsg(endpointId, onGameEndId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_GAME_END), onGameEnd, nullptr);
}

MSGPI_EXPORT void MSGPIAPI PluginUnload()
{
   assert(server == nullptr);
   if (vpxApi == nullptr)
      return;
   
   msgApi->UnsubscribeMsg(onGameStartId, onGameStart);
   msgApi->UnsubscribeMsg(onGameEndId, onGameEnd);
   msgApi->ReleaseMsgID(onGameStartId);
   msgApi->ReleaseMsgID(onGameEndId);
   msgApi->ReleaseMsgID(onPrepareFrameId);
   vpxApi = nullptr;
   msgApi = nullptr;
}

void RegisterB2SPlugin()
{
   auto b2sPlugin = MsgPluginManager::GetInstance().RegisterPlugin(
      "B2S", "B2S Backglass renderer", "A plugin to render B2S backglass files", "", "0.1", "https://www.github.com/vpinball", PluginLoad, PluginUnload);
   b2sPlugin->Load(&MsgPluginManager::GetInstance().GetMsgAPI());
}
