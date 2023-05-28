#pragma once

#define HEADTRACKER_FREETRACKER2
#define HEADTRACKER_UDP
#define HEADTRACKER_BAM

#include "Eigen/Dense"

class HeadTracker final
{
public:
   HeadTracker();
   ~HeadTracker();
   bool Update(vec3& position);

   void UpdateCalibration();
   void GetLastAcquired(vec3& position) const;

private:
   vec3 m_lastAcquiredPos = vec3(0.f, 0.f, 0.f);
   Eigen::Matrix3f m_rotation;
   Eigen::Vector3f m_translation;
   float m_scale;

   ///////////////////////////////////////////////////////////////////
   // UDP (Opentrack UDP communication for Window, Mac and Linux)
   #ifdef HEADTRACKER_UDP
   bool UpdateUDP();
   void ReleaseUDP();
   #endif
   
    
   ///////////////////////////////////////////////////////////////////
   // BAM (Windows mapped memory communication)
   #ifdef HEADTRACKER_BAM
   bool UpdateBAM();
   void ReleaseBAM();

   /// Single captured data about player position.
   struct BAMPlayerData
   {
      double StartPosition[4]; // x,y,z [mm] + timestamp [ms]
      double EndPosition[4]; // x,y,z [mm] + timestamp [ms]
      double EyeVec[3]; // [normalized vector]
      int FrameCounter;
   };

   /// Data recived from BAM Tracker.
   struct BAMData
   {
      // values for HRTimer to synchonize tracker and client timers.
      LARGE_INTEGER Time_StartValue;
      double Time_OneMillisecond;

      // Size of screen in millimeters.
      double ScreenWidth, ScreenHeight;

      // Head Tracking Data is double buffered
      BAMPlayerData Data[2];
      int UsedDataSlot; // info in what buffer is last head tracking data
   };

   BAMData* m_BAMData = nullptr;
   HANDLE m_BAMMemMap = 0;
   #endif

   ///////////////////////////////////////////////////////////////////
   // FreeTracker 2 (Windows mapped memory communication)
   #ifdef HEADTRACKER_FREETRACKER2
   bool UpdateFreeTracker();
   void ReleaseFreeTracker();

   /* only 6 headpose floats and the data id are filled -sh */
   typedef struct FTData__
   {
      uint32_t DataID;
      int32_t CamWidth;
      int32_t CamHeight;
      /* virtual pose */
      float Yaw, Pitch, Roll; /* positive yaw to the left, pitch up, rool to the left */
      float X, Y, Z;
      /* raw pose with no smoothing, sensitivity, response curve etc. */
      float RawYaw, RawPitch, RawRoll;
      float RawX, RawY, RawZ;
      /* raw points, sorted by Y, origin top left corner */
      float X1, Y1, X2, Y2, X3, Y3, X4, Y4;
   } volatile FTData;

   typedef struct FTHeap__
   {
      FTData data;
      int32_t GameID;
      union
      {
         unsigned char table[8];
         int32_t table_ints[2];
      };
      int32_t GameID2;
   } volatile FTHeap;

   FTHeap* m_FTData = nullptr;
   HANDLE m_FTMemMap = 0;
   HANDLE m_FTMutex = 0;
   #endif
};
