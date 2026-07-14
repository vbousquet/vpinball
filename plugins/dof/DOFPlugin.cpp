// license:GPLv3+

#include <cassert>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <charconv>
#include <thread>
#include <mutex>
#include <format>
#if defined(__APPLE__) || defined(__linux__) || defined(__ANDROID__)
#include <pthread.h>
#endif

#include "plugins/VPXPlugin.h"
#include "plugins/ControllerPlugin.h"
#include "plugins/LoggingPlugin.h"

#pragma warning(push)
#pragma warning(disable : 4251) // xxx needs dll-interface
#include "DOF/DOF.h"
#pragma warning(pop)

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <locale>
#endif

using namespace std;

///////////////////////////////////////////////////////////////////////////////
//
// Direct Output Framework plugin
//
// TODO the polling system needs to be extended to also listen for B2S controller

namespace DOFPlugin {

static const MsgPluginAPI* msgApi = nullptr;
static VPXPluginAPI* vpxApi = nullptr;
static uint32_t endpointId;

static unsigned int onControllerGameStartId;
static unsigned int onControllerGameEndId;

static unsigned int getStateSrcId;
static unsigned int onStateSrcChangedId;

static std::mutex sourceMutex;
static bool isRunning = false;
static bool isReady = false;
static StateSrcId pinmameStateSrc = {};
static StateSrcId b2sStateSrc = {};

static std::thread pollThread;

static DOF::DOF* pDOF = nullptr;

static void OnPollStates(void* userData);

LPI_USE_CPP();
#define LOGD DOFPlugin::LPI_LOGD_CPP
#define LOGI DOFPlugin::LPI_LOGI_CPP
#define LOGW DOFPlugin::LPI_LOGW_CPP
#define LOGE DOFPlugin::LPI_LOGE_CPP

LPI_IMPLEMENT_CPP // Implement shared log support

void LIBDOFCALLBACK OnDOFLog(DOF_LogLevel logLevel, const char* format, va_list args)
{
   va_list args_copy;
   va_copy(args_copy, args);
   int size = vsnprintf(nullptr, 0, format, args_copy);
   va_end(args_copy);
   if (size > 0) {
      string buffer(size + 1, '\0');
      vsnprintf(buffer.data(), size + 1, format, args);
      buffer.pop_back(); // remove null terminator
      switch(logLevel) {
         case DOF_LogLevel_INFO:
            LOGI(buffer);
            break;
         case DOF_LogLevel_DEBUG:
            LOGD(buffer);
            break;
         case DOF_LogLevel_ERROR:
            LOGE(buffer);
            break;
         default:
            break;
      }
   }
}

#ifdef _WIN32
static void SetThreadName(const std::string& name)
{
   const int size_needed = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, nullptr, 0);
   if (size_needed <= 1)
      return;
   std::wstring wstr(size_needed - 1, L'\0');
   if (MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wstr.data(), size_needed) == 0)
      return;
   HRESULT hr = SetThreadDescription(GetCurrentThread(), wstr.c_str());
}
#else
static void SetThreadName(const std::string& name)
{
#ifdef __APPLE__
   pthread_setname_np(name.c_str());
#elif defined(__linux__) || defined(__ANDROID__)
   pthread_setname_np(pthread_self(), name.c_str());
#endif
}
#endif

static void PollThread(const string& tablePath, const string& gameId)
{
   assert(pDOF != nullptr);
   SetThreadName("DOF.PollThread"s);
   pDOF->Init(tablePath.c_str(), gameId.c_str());
   bool isInitialState = true;
   vector<bool> pinmameStates;
   while (isRunning)
   {
      {
         std::lock_guard lock(sourceMutex);

         if (!isReady)
         {
            for (unsigned int i = 0; i < b2sStateSrc.nStates; i++)
            {
               float state = 0.f; b2sStateSrc.GetState(i, CTLPI_STATE_TYPE_FLOAT, &state);
               pDOF->DataReceive('E', b2sStateSrc.stateDefs[i].id.stateId, state > 0.5f ? 1 : 0);
            }
            isReady = true;
         }

         isInitialState |= pinmameStates.size() != pinmameStateSrc.nStates;

         pinmameStates.resize(pinmameStateSrc.nStates);
         for (unsigned int i = 0; i < pinmameStateSrc.nStates; i++)
         {
            char type;
            switch (pinmameStateSrc.stateDefs[i].id.groupId & 0xFF00)
            {
            case 0x0000: type = 'S'; break; // Solenoids
            case 0x0100: type = 'G'; break; // GI
            case 0x0200: type = 'L'; break; // Lamps
            case 0x0300: type = '\0'; break; // Mech
            case 0x0400: type = 'W'; break; // Switch
            case 0x0500: type = '\0'; break; // Dip Switch
            default: type = '\0'; break; // Unsupported
            }
            if (type != '\0')
            {
               float state = 0.f; pinmameStateSrc.GetState(i, CTLPI_STATE_TYPE_FLOAT, &state);
               if (isInitialState || (pinmameStates[i] && state < 0.25f) || (!pinmameStates[i] && state > 0.75f))
               {
                  pDOF->DataReceive(type, pinmameStateSrc.stateDefs[i].id.stateId, state > 0.5f ? 1 : 0);
                  pinmameStates[i] = state > 0.5f;
               }
            }
         }

         isInitialState = false;
      }
      
      // Fixed update at 60 FPS
      std::this_thread::sleep_for(std::chrono::microseconds(16666));
   }
   {
      std::lock_guard lock(sourceMutex);
      isReady = false;
   }
   pDOF->Finish();
}

static void MSGPIAPI OnB2SStateChg(unsigned int index, void* context)
{
   std::lock_guard lock(sourceMutex);
   if (isReady && index < b2sStateSrc.nStates && pDOF != nullptr)
   {
      float state = 0.f; b2sStateSrc.GetState(index, CTLPI_STATE_TYPE_FLOAT, &state);
      pDOF->DataReceive('E', b2sStateSrc.stateDefs[index].id.stateId, state > 0.5f ? 1 : 0);
   }
}

static void OnControllerGameStart(const unsigned int eventId, void* userData, void* msgData)
{
   const CtlOnGameStateChgMsg* msg = static_cast<const CtlOnGameStateChgMsg*>(msgData);
   assert(msg != nullptr && msg->gameId != nullptr);

   // FIXME implement multiple ontroller sources (PinMAME, B2S, PuP, ...)

   // FIXME: Temp fix for issues 3298, 3309, and maybe 3322?
   if (isRunning)
   {
      LOGW("Ignoring game start, already running"s);
      return;
   }

   if (pDOF) {
      LOGI("OnControllerGameStart: gameId="s + msg->gameId);
      isRunning = true;
      VPXTableInfo tableInfo;
      vpxApi->GetTableInfo(&tableInfo);
      string path = tableInfo.path;
      string gameId = msg->gameId;
      pollThread = std::thread(PollThread, path, gameId);
   }
}

static void OnControllerGameEnd(const unsigned int eventId, void* userData, void* msgData)
{
   const CtlOnGameStateChgMsg* msg = static_cast<const CtlOnGameStateChgMsg*>(msgData);
   assert(msg != nullptr && msg->gameId != nullptr);

   // FIXME implement multiple controller sources (PinMAME, B2S, PuP, ...)

   if (pDOF)
   {
      LOGI("OnControllerGameEnd"s);
      isRunning = false;
      if (pollThread.joinable())
         pollThread.join();
   }
}

static void ClearStates()
{
   delete[] pinmameStateSrc.stateDefs;
   memset(&pinmameStateSrc, 0, sizeof(pinmameStateSrc));
   for (unsigned int i = 0; i < b2sStateSrc.nStates; i++)
      if (b2sStateSrc.SetChangeCallback)
         b2sStateSrc.SetChangeCallback(i, 0, OnB2SStateChg, nullptr);
   delete[] b2sStateSrc.stateDefs;
   memset(&b2sStateSrc, 0, sizeof(b2sStateSrc));
}

static void OnStateSrcChanged(const unsigned int eventId, void* userData, void* msgData)
{
   std::lock_guard lock(sourceMutex);
   ClearStates();

   GetStateSrcMsg getSrcMsg = { 0, 0, nullptr };
   msgApi->BroadcastMsg(endpointId, getStateSrcId, &getSrcMsg);
   if (getSrcMsg.count == 0)
   {
      LOGI("OnStateSrcChanged - No source"s);
      return;
   }

   getSrcMsg = { getSrcMsg.count, 0, new StateSrcId[getSrcMsg.count] };
   msgApi->BroadcastMsg(endpointId, getStateSrcId, &getSrcMsg);
   MsgEndpointInfo info;
   for (unsigned int i = 0; i < getSrcMsg.count; i++)
   {
      memset(&info, 0, sizeof(info));
      msgApi->GetEndpointInfo(getSrcMsg.entries[i].id.endpointId, &info);
      if (info.id != nullptr && info.id == "PinMAME"sv)
      {
         pinmameStateSrc = getSrcMsg.entries[i];
         if (pinmameStateSrc.stateDefs)
         {
            pinmameStateSrc.stateDefs = new StateDef[pinmameStateSrc.nStates];
            memcpy(pinmameStateSrc.stateDefs, getSrcMsg.entries[i].stateDefs, getSrcMsg.entries[i].nStates * sizeof(StateDef));
         }
      }
      else if (info.id != nullptr && (info.id == "B2S"sv || info.id == "B2SLegacy"sv))
      {
         b2sStateSrc = getSrcMsg.entries[i];
         if (b2sStateSrc.stateDefs)
         {
            b2sStateSrc.stateDefs = new StateDef[b2sStateSrc.nStates];
            memcpy(b2sStateSrc.stateDefs, getSrcMsg.entries[i].stateDefs, getSrcMsg.entries[i].nStates * sizeof(StateDef));
         }
         for (unsigned int j = 0; j < b2sStateSrc.nStates; j++)
            if (b2sStateSrc.SetChangeCallback)
               b2sStateSrc.SetChangeCallback(j, 1, OnB2SStateChg, nullptr);
      }
   }
   delete[] getSrcMsg.entries;

   LOGI(std::format("OnStateSrcChanged - Found {} PinMAME states and {} B2S states", pinmameStateSrc.nStates, b2sStateSrc.nStates));
}

}

using namespace DOFPlugin;

MSGPI_EXPORT void MSGPIAPI DOFPluginLoad(const uint32_t sessionId, const MsgPluginAPI* api)
{
   msgApi = api;
   endpointId = sessionId;

   LPISetup(endpointId, msgApi);

   memset(&pinmameStateSrc, 0, sizeof(pinmameStateSrc));
   ClearStates();

   unsigned int getVpxApiId = msgApi->GetMsgID(VPXPI_NAMESPACE, VPXPI_MSG_GET_API);
   msgApi->BroadcastMsg(endpointId, getVpxApiId, &vpxApi);
   msgApi->ReleaseMsgID(getVpxApiId);

   msgApi->SubscribeMsg(endpointId, onControllerGameStartId = msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_EVT_ON_GAME_START), OnControllerGameStart, nullptr);
   msgApi->SubscribeMsg(endpointId, onControllerGameEndId = msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_EVT_ON_GAME_END), OnControllerGameEnd, nullptr);

   getStateSrcId = msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_STATE_GET_SRC_MSG);
   onStateSrcChangedId = msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_STATE_ON_SRC_CHG_MSG);

   msgApi->SubscribeMsg(endpointId, onStateSrcChangedId, OnStateSrcChanged, nullptr);

   OnStateSrcChanged(onStateSrcChangedId, nullptr, nullptr);

   VPXInfo vpxInfo;
   vpxApi->GetVpxInfo(&vpxInfo);

   DOF::Config* pConfig = DOF::Config::GetInstance();
   pConfig->SetLogCallback(OnDOFLog);
   pConfig->SetBasePath(vpxInfo.prefPath);

   pDOF = new DOF::DOF();
}

MSGPI_EXPORT void MSGPIAPI DOFPluginUnload()
{
   isRunning = false;
   if (pollThread.joinable())
      pollThread.join();

   ClearStates();

   delete pDOF;
   pDOF = nullptr;

   msgApi->UnsubscribeMsg(onControllerGameStartId, OnControllerGameStart, nullptr);
   msgApi->UnsubscribeMsg(onControllerGameEndId, OnControllerGameEnd, nullptr);
   msgApi->UnsubscribeMsg(onStateSrcChangedId, OnStateSrcChanged, nullptr);

   msgApi->ReleaseMsgID(onControllerGameStartId);
   msgApi->ReleaseMsgID(onControllerGameEndId);
   msgApi->ReleaseMsgID(getStateSrcId);
   msgApi->ReleaseMsgID(onStateSrcChangedId);

   msgApi = nullptr;
}
