// license:GPLv3+

#pragma once

#include "core/player.h"

class PerfUI final
{
public:
   PerfUI(Player* const player);
   ~PerfUI();

   void SetDPI(float dpi) { m_dpi = dpi; }

   enum PerfMode
   {
      PM_DISABLED,
      PM_FPS,
      PM_STATS
   };
   void NextPerfMode();
   PerfMode GetPerfMode() const { return m_showPerf; }
   void SetPerfMode(PerfMode mode) { m_showPerf = mode; }

   void Update();

   class PlotData
   {
   public:
      PlotData();

      void SetRolling(bool rolling);
      void AddPoint(const float x, const float y);
      bool HasData() const;
      ImVec2 GetLast() const;
      float GetMovingMax() const;

   public:
      int m_offset = 0;
      float m_timeSpan = 2.5f;
      ImVector<ImVec2> m_data;
      bool m_rolling = true;
      float m_movingMax = 0.f;

   private:
      const int m_maxSize;
   };

private:
   Player* const m_player;
   float m_dpi = 1.0f;

   PerfMode m_showPerf = PerfMode::PM_DISABLED;
   bool m_showAvgFPS = true;
   bool m_showRollingFPS = true;
   
   PlotData m_plotFPS;
   PlotData m_plotFPSSmoothed;
   PlotData m_plotPhysx;
   PlotData m_plotPhysxSmoothed;
   PlotData m_plotScript;
   PlotData m_plotScriptSmoothed;
};
