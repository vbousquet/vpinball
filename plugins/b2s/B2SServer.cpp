// license:GPLv3+

#include "common.h"

#include "B2SServer.h"

namespace B2S {

B2SServer* B2SServer::m_singleton = nullptr;

B2SServer::B2SServer(const MsgPluginAPI* const msgApi, unsigned int endpointId, const VPXPluginAPI* const vpxApi, ScriptClassDef* serverClassDef)
   : m_controllerClassProxy(msgApi, endpointId, "PinMAME_", "PinMAME_Controller", "B2S_", serverClassDef)
   , m_controllerProxy(m_controllerClassProxy)
   , m_msgApi(msgApi)
   , m_endpointId(endpointId)
   , m_vpxApi(vpxApi)
   , m_onGetAuxRendererId(msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_MSG_GET_AUX_RENDERER))
   , m_onAuxRendererChgId(msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_AUX_RENDERER_CHG))
   , m_ancillaryRendererDef({ "B2S", "B2S Backglass & FullDMD", "Renderer for directb2s backglass files", this, OnRender })
   , m_onGameStartId(msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_EVT_ON_GAME_START))
   , m_onGameEndId(msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_EVT_ON_GAME_END))
   , m_onGetStateSrcId(msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_STATE_GET_SRC_MSG))
   , m_onStateSrcChgId(msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_STATE_ON_SRC_CHG_MSG))
{
   m_singleton = this;

   VPXTableInfo tableInfo;
   m_vpxApi->GetTableInfo(&tableInfo);

   // Search for an exact match (same file name with .directb2s extension)
   const std::filesystem::path tablePath(tableInfo.path);
   std::filesystem::path b2sFilename = find_case_insensitive_file_path(tablePath.parent_path() / tablePath.filename().replace_extension(".directb2s"));

   // Search for a file matching the template 'foldername.directb2s' for file layout where tables are located in a folder with their companion files (b2s, pup, flex, music, ...)
   if (b2sFilename.empty())
   {
      std::filesystem::path folderName = tablePath.parent_path().filename();
      folderName += ".directb2s"sv;
      b2sFilename = find_case_insensitive_file_path(tablePath.parent_path() / folderName);
   }

   if (!b2sFilename.empty())
   {
      auto loadFile = [](const std::filesystem::path& path)
      {
         std::shared_ptr<B2STable> b2s;
         try
         {
            tinyxml2::XMLDocument b2sTree;
            b2sTree.LoadFile(path.string().c_str());
            if (b2sTree.FirstChildElement("DirectB2SData"))
               b2s = std::make_shared<B2STable>(*b2sTree.FirstChildElement("DirectB2SData"));
         }
         catch (...)
         {
            LOGE("Failed to load B2S file: " + path.string());
         }
         return b2s;
      };
      // B2S file format is heavily unoptimized so perform loading asynchronously (all assets are directly included in the XML file using Base64 encoding)
      m_loadedB2S = std::async(std::launch::async, loadFile, b2sFilename);
   }

   m_msgApi->SubscribeMsg(m_endpointId, m_onGetAuxRendererId, OnGetRenderer, this);
   m_msgApi->BroadcastMsg(m_endpointId, m_onAuxRendererChgId, nullptr);

   m_stateSrc.id.endpointId = m_endpointId;
   m_stateSrc.nGroups = 0;
   m_stateSrc.groupDefs = nullptr;
   m_stateSrc.GetState = GetStateAPI;
   m_stateSrc.SetState = SetStateAPI;
   m_stateSrc.SetChangeCallback = RegisterStateChangeCallback;
   m_msgApi->SubscribeMsg(m_endpointId, m_onGetStateSrcId, OnGetStateSrc, this);
   UpdateStateSrc();
}

B2SServer::~B2SServer()
{
   if (m_loadedB2S.valid())
      m_loadedB2S.get();
   m_renderer = nullptr;

   if (m_gameRunning)
   {
      CtlOnGameStateChgMsg msg = { m_endpointId, m_b2sName.c_str(), 0 };
      m_msgApi->BroadcastMsg(m_endpointId, m_onGameEndId, reinterpret_cast<void*>(&msg));
   }
   m_msgApi->ReleaseMsgID(m_onGameStartId);
   m_msgApi->ReleaseMsgID(m_onGameEndId);

   if (m_states.size() > 0)
   {
      m_states.clear();
      UpdateStateSrc();
   }

   m_msgApi->UnsubscribeMsg(m_onGetAuxRendererId, OnGetRenderer, this);
   m_msgApi->BroadcastMsg(m_endpointId, m_onAuxRendererChgId, nullptr);
   m_msgApi->ReleaseMsgID(m_onGetAuxRendererId);
   m_msgApi->ReleaseMsgID(m_onAuxRendererChgId);

   m_msgApi->UnsubscribeMsg(m_onGetStateSrcId, OnGetStateSrc, this);
   m_msgApi->ReleaseMsgID(m_onGetStateSrcId);
   m_msgApi->ReleaseMsgID(m_onStateSrcChgId);
   
   if (m_onDestroyHandler)
      m_onDestroyHandler(this);
   
   m_singleton = nullptr;
}

string B2SServer::GetB2SName() const { return m_b2sName; }

void B2SServer::SetB2SName(const std::string& b2sName) { 
   if (b2sName == m_b2sName)
      return;
   if (m_gameRunning)
   {
      CtlOnGameStateChgMsg msg = { m_endpointId, m_b2sName.c_str(), 0 };
      m_msgApi->BroadcastMsg(m_endpointId, m_onGameEndId, reinterpret_cast<void*>(&msg));
   }
   m_b2sName = b2sName;
   if (m_gameRunning)
   {
      CtlOnGameStateChgMsg msg = { m_endpointId, m_b2sName.c_str(), 0 };
      m_msgApi->BroadcastMsg(m_endpointId, m_onGameStartId, reinterpret_cast<void*>(&msg));
   }
}

void B2SServer::OnGetStateSrc(const unsigned int, void* userData, void* msgData)
{
   auto me = static_cast<B2SServer*>(userData);
   auto msg = static_cast<GetStateSrcMsg*>(msgData);

   if (msg->count < msg->maxEntryCount)
      memcpy(&msg->entries[msg->count], &me->m_stateSrc, sizeof(StateSrcId));
   msg->count++;
}

void B2SServer::UpdateStateSrc()
{
   if (m_gameRunning && m_states.empty())
   {
      m_gameRunning = false;
      CtlOnGameStateChgMsg msg = { m_endpointId, m_b2sName.c_str(), 0 };
      m_msgApi->BroadcastMsg(m_endpointId, m_onGameEndId, reinterpret_cast<void*>(&msg));
   }
   else if (!m_gameRunning && !m_states.empty())
   {
      m_gameRunning = true;
      CtlOnGameStateChgMsg msg = { m_endpointId, m_b2sName.c_str(), 0 };
      m_msgApi->BroadcastMsg(m_endpointId, m_onGameStartId, reinterpret_cast<void*>(&msg));
   }

   delete[] m_stateSrc.stateDefs;
   m_stateSrc.nStates = static_cast<unsigned int>(m_states.size());
   m_stateSrc.stateDefs = new StateDef[m_stateSrc.nStates];
   m_stateSrcNames.resize(m_stateSrc.nStates);
   uint16_t index = 0;
   for (const auto& [id, v] : m_states)
   {
      m_stateSrcNames[index] = std::format("B2S.Data #{}", id);
      m_stateSrc.stateDefs[index].name = m_stateSrcNames[index].c_str();
      m_stateSrc.stateDefs[index].desc = nullptr;
      m_stateSrc.stateDefs[index].id.groupId = 0x0001;
      m_stateSrc.stateDefs[index].id.stateId = static_cast<uint16_t>(id);
      m_stateSrc.stateDefs[index].type = CTLPI_STATE_TYPE_FLOAT;
      m_stateSrc.stateDefs[index].writable = 0;
      index++;
   }
   m_stateChgCallbacks.clear();
   m_msgApi->BroadcastMsg(m_endpointId, m_onStateSrcChgId, nullptr);
}

int MSGPIAPI B2SServer::GetStateAPI(unsigned int inputIndex, int type, void* pResult)
{
   if (B2SServer::m_singleton == nullptr || inputIndex >= m_singleton->m_stateSrc.nStates)
      return -1;
   int b2sId = m_singleton->m_stateSrc.stateDefs[inputIndex].id.stateId;
   float val = m_singleton->GetState(b2sId);
   if (type == CTLPI_STATE_TYPE_FLOAT)
      *static_cast<float*>(pResult) = val;
   else if (type == CTLPI_STATE_TYPE_UINT8)
      *static_cast<uint8_t*>(pResult) = static_cast<uint8_t>(val * 255.f);
   else if (type == CTLPI_STATE_TYPE_BOOL)
      *static_cast<bool*>(pResult) = val != 0.f;
   else
      return -1;
   return 0;
}

int MSGPIAPI B2SServer::SetStateAPI(unsigned int inputIndex, int type, void* pResult)
{
   return -1;
}

void MSGPIAPI B2SServer::RegisterStateChangeCallback(unsigned int deviceIndex, int isRegister, ctlpi_chg_callback cb, void* ctx)
{
   if (B2SServer::m_singleton == nullptr || deviceIndex >= m_singleton->m_stateSrc.nStates)
   {
      LOGE("Invalid state listener registration requested"s);
      assert(false);
      return;
   }
   const int b2sId = m_singleton->m_stateSrc.stateDefs[deviceIndex].id.stateId;
   if (auto mapIt = m_singleton->m_stateChgCallbacks.find(b2sId); mapIt == m_singleton->m_stateChgCallbacks.end())
      m_singleton->m_stateChgCallbacks[b2sId] = vector<ChgCallback>();
   auto& callbacks = m_singleton->m_stateChgCallbacks[b2sId];
   auto it = std::ranges::find_if(callbacks, [&cb, ctx](const ChgCallback& a) { return a.m_callback == cb && a.m_context == ctx; });
   if (isRegister)
   {
      if (it != callbacks.end())
      {
         LOGE("Requested to register an already registered state change listener"s);
      }
      else
      {
         callbacks.emplace_back(cb, deviceIndex, ctx);
      }
   }
   else
   {
      if (it != callbacks.end())
      {
         callbacks.erase(it);
      }
      else
      {
         LOGE("Requested to unregister an unknown state change listener"s);
      }
   }
}

int B2SServer::OnRender(VPXRenderContext2D* ctx, void* userData)
{
   if ((ctx->window != VPXWindowId::VPXWINDOW_Backglass) && (ctx->window != VPXWindowId::VPXWINDOW_ScoreView))
      return false;

   auto me = static_cast<B2SServer*>(userData);
   if (me->m_renderer)
      return me->m_renderer->Render(ctx, me);

   if (me->m_loadedB2S.valid())
   {
      if (me->m_loadedB2S.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
      {
         me->m_renderer = std::make_unique<B2SRenderer>(me->m_msgApi, me->m_endpointId, me->m_loadedB2S.get());
         me->m_renderer->Render(ctx, me);
      }
      return true; // Until loaded, we assume that the file will succeed loading with the expected backglass/score view
   }

   return false;
}

void B2SServer::OnGetRenderer(const unsigned int, void* userData, void* msgData)
{
   auto me = static_cast<B2SServer*>(userData);
   auto msg = static_cast<GetAncillaryRendererMsg*>(msgData);
   if ((msg->window == VPXWindowId::VPXWINDOW_Backglass) || (msg->window == VPXWindowId::VPXWINDOW_ScoreView))
   {
      if (msg->count < msg->maxEntryCount)
         msg->entries[msg->count] = me->m_ancillaryRendererDef;
      msg->count++;
   }
}



void B2SServer::B2SSetData(int b2sId, int value)
{
   LOGD(std::format("B2SSetData {}={}", b2sId, value));
   const auto it = m_states.find(b2sId);
   if (it == m_states.end())
   {
      m_states[b2sId] = static_cast<float>(value);
      UpdateStateSrc();
   }
   else
      it->second = static_cast<float>(value);

   const auto chgIt = m_stateChgCallbacks.find(b2sId);
   if (chgIt != m_stateChgCallbacks.end())
   {
      for (const auto& cb : chgIt->second)
         cb.m_callback(cb.m_index, cb.m_context);
   }
}

void B2SServer::B2SSetData(const std::string& group, int value)
{

}

float B2SServer::GetState(int b2sId) const
{
   const auto it = m_states.find(b2sId);
   return it == m_states.end() ? 0.f : it->second;
}

// Scores

void B2SServer::B2SSetScorePlayer(int playerno, int score)
{
   m_playerScores[playerno] = score;
}

int B2SServer::GetPlayerScore(int player) const
{
   const auto it = m_playerScores.find(player);
   return it == m_playerScores.end() ? 0 : it->second;
}

void B2SServer::B2SSetScoreDigit(int digit, int value)
{
   m_scoreDigits[digit] = value;
}

int B2SServer::GetScoreDigit(int b2sId) const
{
   const auto it = m_scoreDigits.find(b2sId);
   return it == m_scoreDigits.end() ? 0 : it->second;
}

}
