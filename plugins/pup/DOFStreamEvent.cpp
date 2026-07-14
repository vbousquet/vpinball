// license:GPLv3+

#include "DOFStreamEvent.h"

#include <cmath>
#include <cstring>
#include <string>
using std::string;
using namespace std::string_literals;
using namespace std::string_view_literals;

using std::vector;

namespace PUP
{

DOFEventStream::DOFEventStream(const MsgPluginAPI* msgApi, uint32_t endpointId, const std::function<void(char, int, int)>& eventHandler)
   : m_endpointId(endpointId) 
   , m_msgApi(msgApi)
   , m_eventHandler(eventHandler)
{
   m_getDmdSrcId = m_msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_DISPLAY_GET_SRC_MSG);
   m_onDmdSrcChangedId = m_msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_DISPLAY_ON_SRC_CHG_MSG);
   
   m_getSegSrcId = m_msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_DISPLAY_GET_SRC_MSG);
   m_onSegSrcChangedId = m_msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_SEG_ON_SRC_CHG_MSG);
   
   m_getStateSrcId = m_msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_STATE_GET_SRC_MSG);
   m_onStateSrcChangedId = m_msgApi->GetMsgID(CTLPI_NAMESPACE, CTLPI_STATE_ON_SRC_CHG_MSG);

   m_onSerumTriggerId = m_msgApi->GetMsgID("Serum", "OnDmdTrigger");

   m_msgApi->SubscribeMsg(m_endpointId, m_onDmdSrcChangedId, OnDMDSrcChanged, this);
   m_msgApi->SubscribeMsg(m_endpointId, m_onSegSrcChangedId, OnSegSrcChanged, this);
   m_msgApi->SubscribeMsg(m_endpointId, m_onStateSrcChangedId, OnStateSrcChanged, this);
   m_msgApi->SubscribeMsg(m_endpointId, m_onSerumTriggerId, OnSerumTrigger, this);
   OnDMDSrcChanged(m_onDmdSrcChangedId, this, nullptr);
   OnStateSrcChanged(m_onStateSrcChangedId, this, nullptr);

   m_thread = std::thread(&DOFEventStream::StatePollingThread, this);
}

DOFEventStream::~DOFEventStream()
{
   m_isRunning = false;
   if (m_thread.joinable())
      m_thread.join();

   for (unsigned int i = 0; i < m_b2sStateSrc.nStates; i++)
      m_b2sStateSrc.SetChangeCallback(i, 0, OnB2SStateChg, this);

   m_msgApi->UnsubscribeMsg(m_onDmdSrcChangedId, OnDMDSrcChanged, this);
   m_msgApi->UnsubscribeMsg(m_onSegSrcChangedId, OnSegSrcChanged, this);
   m_msgApi->UnsubscribeMsg(m_onStateSrcChangedId, OnStateSrcChanged, this);
   m_msgApi->UnsubscribeMsg(m_onSerumTriggerId, OnSerumTrigger, this);
   delete[] m_b2sStateSrc.stateDefs;
   delete[] m_pmStateSrc.stateDefs;

   m_msgApi->ReleaseMsgID(m_onSerumTriggerId);

   m_msgApi->ReleaseMsgID(m_getStateSrcId);
   m_msgApi->ReleaseMsgID(m_onStateSrcChangedId);

   m_msgApi->ReleaseMsgID(m_getSegSrcId);
   m_msgApi->ReleaseMsgID(m_onSegSrcChangedId);

   m_msgApi->ReleaseMsgID(m_getDmdSrcId);
   m_msgApi->ReleaseMsgID(m_onDmdSrcChangedId);
}

void DOFEventStream::SetDMDHandler(const std::function<DisplaySrcId(const GetDisplaySrcMsg&)>& select, const std::function<int(const DisplaySrcId&, const uint8_t*)>& process)
{
   m_selectDmd = select;
   m_processDmd = process;
   OnDMDSrcChanged(m_onDmdSrcChangedId, this, nullptr);
}

// Broadcasted by Serum plugin when frame triggers are identified
void DOFEventStream::OnSerumTrigger(const unsigned int eventId, void* userData, void* eventData)
{
   auto me = static_cast<DOFEventStream*>(userData);
   auto trigger = static_cast<unsigned int*>(eventData);
   me->QueueEvent('D', static_cast<int>(*trigger), 1);
   me->QueueEvent('D', static_cast<int>(*trigger), 0);
}

void DOFEventStream::OnDMDSrcChanged(const unsigned int eventId, void* userData, void* eventData)
{
   auto me = static_cast<DOFEventStream*>(userData);
   std::lock_guard lock(me->m_pollSrcMutex);
   me->m_dmdId.id.id = 0;
   GetDisplaySrcMsg getSrcMsg = { 0, 0, nullptr };
   me->m_msgApi->BroadcastMsg(me->m_endpointId, me->m_getDmdSrcId, &getSrcMsg);
   getSrcMsg = { getSrcMsg.count, 0, new DisplaySrcId[getSrcMsg.count] };
   me->m_msgApi->BroadcastMsg(me->m_endpointId, me->m_getDmdSrcId, &getSrcMsg);
   me->m_dmdId = me->m_selectDmd(getSrcMsg);
   me->m_lastDmdFrameId = 0;
   delete[] getSrcMsg.entries;
}

void DOFEventStream::OnSegSrcChanged(const unsigned int eventId, void* userData, void* eventData)
{
   auto me = static_cast<DOFEventStream*>(userData);
   std::lock_guard lock(me->m_pollSrcMutex);
   
   // PinMAME controller
   me->m_pmSegSrc.clear();
   me->m_pmLastSegFrame.clear();
   me->m_pmLastSegFrameId.clear();
   if (unsigned int pinmameEndPoint = me->m_msgApi->GetPluginEndpoint("PinMAME"); pinmameEndPoint)
   {
      me->m_pmSegSrc.resize(1024);
      GetSegSrcMsg getSrcMsg = { static_cast<unsigned int>(me->m_pmSegSrc.size()), 0, me->m_pmSegSrc.data() };
      me->m_msgApi->SendMsg(me->m_endpointId, me->m_getSegSrcId, pinmameEndPoint, &getSrcMsg);
      me->m_pmSegSrc.resize(getSrcMsg.count);
      me->m_pmLastSegFrame.resize(getSrcMsg.count);
      me->m_pmLastSegFrameId.resize(getSrcMsg.count);
   }
}

void DOFEventStream::OnStateSrcChanged(const unsigned int eventId, void* userData, void* eventData)
{
   auto me = static_cast<DOFEventStream*>(userData);
   std::unique_lock lock(me->m_pollSrcMutex);

   // PinMAME controller
   delete[] me->m_pmStateSrc.stateDefs;
   memset(&me->m_pmStateSrc, 0, sizeof(me->m_pmStateSrc));
   me->m_pmStates.clear();
   if (unsigned int pinmameEndPoint = me->m_msgApi->GetPluginEndpoint("PinMAME"); pinmameEndPoint)
   {
      GetStateSrcMsg getSrcMsg = { 1, 0, &me->m_pmStateSrc };
      me->m_msgApi->SendMsg(me->m_endpointId, me->m_getStateSrcId, pinmameEndPoint, &getSrcMsg);
      if (getSrcMsg.count && me->m_pmStateSrc.stateDefs)
      {
         // Copy device definitions
         StateDef* devices = new StateDef[me->m_pmStateSrc.nStates];
         memcpy(devices, me->m_pmStateSrc.stateDefs, me->m_pmStateSrc.nStates * sizeof(StateDef));
         me->m_pmStateSrc.stateDefs = devices;
         me->m_pmStates.resize(me->m_pmStateSrc.nStates, -1);
      }
   }

   // B2S controller
   delete[] me->m_b2sStateSrc.stateDefs;
   memset(&me->m_b2sStateSrc, 0, sizeof(me->m_b2sStateSrc));
   unsigned int b2sEndPoint = me->m_msgApi->GetPluginEndpoint("B2S");
   if (!b2sEndPoint)
      b2sEndPoint = me->m_msgApi->GetPluginEndpoint("B2SLegacy");
   if (b2sEndPoint)
   {
      GetStateSrcMsg getSrcMsg = { 1, 0, &me->m_b2sStateSrc };
      me->m_msgApi->SendMsg(me->m_endpointId, me->m_getStateSrcId, b2sEndPoint, &getSrcMsg);
      if (getSrcMsg.count && me->m_b2sStateSrc.stateDefs)
      {
         // Copy device definitions and register state change listener
         StateDef* devices = new StateDef[me->m_b2sStateSrc.nStates];
         memcpy(devices, me->m_b2sStateSrc.stateDefs, me->m_b2sStateSrc.nStates * sizeof(StateDef));
         me->m_b2sStateSrc.stateDefs = devices;
         for (unsigned int i = 0; i < me->m_b2sStateSrc.nStates; i++)
            me->m_b2sStateSrc.SetChangeCallback(i, 1, OnB2SStateChg, me);
      }
   }

   lock.unlock();

   for (unsigned int i = 0; i < me->m_b2sStateSrc.nStates; i++)
      OnB2SStateChg(i, me);
}

void MSGPIAPI DOFEventStream::OnB2SStateChg(unsigned int index, void* context)
{
   // E: B2S Controller generic input state (B2SSetData / B2SPulseData)
   auto me = static_cast<DOFEventStream*>(context);
   float state;
   me->m_b2sStateSrc.GetState(index, CTLPI_STATE_TYPE_FLOAT, &state);
   me->QueueEvent('E', static_cast<int>(me->m_b2sStateSrc.stateDefs[index].id.stateId), state > 0.5f ? 1 : 0);

   // B: B2S Controller score digit
   // TODO implement

   // C: B2S Controller score
   // TODO implement
}

// Update thread that poll analog sources (lamps, solenoids, ...) at 60Hz
void DOFEventStream::StatePollingThread()
{
   //SetThreadName("DOFEventStream.StatePollThread"s);
   while (m_isRunning)
   {
      std::this_thread::sleep_for(std::chrono::microseconds(16666));
      if (!m_isRunning)
         break;

      std::lock_guard lock(m_pollSrcMutex);

      // D: DMD frame identification
      if (m_dmdId.id.id != 0)
      {
         DisplayFrame dmdFrame = m_dmdId.GetIdentifyFrame(m_dmdId.id);
         if (dmdFrame.frame && dmdFrame.frameId != m_lastDmdFrameId)
         {
            m_lastDmdFrameId = dmdFrame.frameId;
            const int dmdTrigger = m_processDmd(m_dmdId, static_cast<const uint8_t*>(dmdFrame.frame));
            if (dmdTrigger > 0)
            {
               QueueEvent('D', dmdTrigger, 1);
               QueueEvent('D', dmdTrigger, 0);
            }
         }
      }
      
      // D: PinMAME Segment display state
      int segIndex = 0;
      int segDisplayIndex = 0;
      for (const auto& segSrc : m_pmSegSrc)
      {
         if (const SegDisplayFrame segFrame = segSrc.GetState(segSrc.id); segFrame.frameId != m_pmLastSegFrameId[segIndex])
         {
            m_pmLastSegFrameId[segIndex] = segFrame.frameId;
            for (unsigned int i = 0; i < segSrc.nElements; i++)
            {
               uint16_t elementState = 0;
               for (int j = 0; j < 16; j++)
                  if (segFrame.frame[j] > 0.5f)
                     elementState |= 1u << j;
               if (elementState != m_pmLastSegFrame[segDisplayIndex])
               {
                  m_pmLastSegFrame[segDisplayIndex] = elementState;
                  QueueEvent('D', segDisplayIndex, elementState);
               }
               segDisplayIndex++;
            }
         }
         else
         {
            segDisplayIndex += segSrc.nElements;
         }
         segIndex++;
      }

      // S: PinMAME solenoid state
      // G: PinMAME GI state
      // L: PinMAME lamp state
      // N: PinMAME mech state
      // W: PinMAME switch events
      for (unsigned int i = 0; i < m_pmStateSrc.nStates; i++)
      {
         float floatState;
         m_pmStateSrc.GetState(i, CTLPI_STATE_TYPE_FLOAT, &floatState);
         const int state = static_cast<int>(roundf(floatState));
         if (m_pmStates[i] != state)
         {
            m_pmStates[i] = state;
            switch (m_pmStateSrc.stateDefs[i].id.groupId & 0xFF00)
            {
            case 0x0000: QueueEvent('S', m_pmStateSrc.stateDefs[i].id.stateId, state); break;
            case 0x0100: QueueEvent('G', m_pmStateSrc.stateDefs[i].id.stateId, state); break;
            case 0x0200: QueueEvent('L', m_pmStateSrc.stateDefs[i].id.stateId, state); break;
            case 0x0300: QueueEvent('N', m_pmStateSrc.stateDefs[i].id.stateId, state); break;
            case 0x0400: QueueEvent('W', m_pmStateSrc.stateDefs[i].id.stateId, state); break;
            }
         }
      }
   }
}

}
