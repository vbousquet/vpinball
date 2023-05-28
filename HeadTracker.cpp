#include "stdafx.h"
#include "HeadTracker.h"

#include "Eigen/Dense"
#include "Eigen/SVD"

HeadTracker::HeadTracker()
{
   UpdateCalibration();
}

HeadTracker::~HeadTracker()
{
   #ifdef HEADTRACKER_BAM
   ReleaseBAM();
   #endif

   #ifdef HEADTRACKER_UDP
   ReleaseUDP();
   #endif

   #ifdef HEADTRACKER_FREETRACKER2
   ReleaseFreeTracker();
   #endif
}

bool HeadTracker::Update(vec3& position)
{
   bool acquired = false;

   #ifdef HEADTRACKER_BAM
   if (!acquired && UpdateBAM())
      acquired = true;
   #endif

   #ifdef HEADTRACKER_UDP
   if (!acquired && UpdateUDP())
      acquired = true;
   #endif

   #ifdef HEADTRACKER_FREETRACKER2
   if (!acquired && UpdateFreeTracker())
      acquired = true;
   #endif

   if (acquired)
   {
      Eigen::Vector3f acqPos(m_lastAcquiredPos.x, m_lastAcquiredPos.y, m_lastAcquiredPos.z);
      Eigen::Vector3f worldPos = (1.f / m_scale) * (m_rotation * acqPos + m_translation);
      //Eigen::Vector3f worldPos = rotation * (scale * acqPos) + translation;
      position = vec3(worldPos(0), worldPos(1), worldPos(2));
      //position = m_lastAcquiredPos;
   }

   return acquired;
}

// Algorithm for fitting points implemented from https://nghiaho.com/?page_id=671
void HeadTracker::UpdateCalibration()
{
   // Get the 3 fitting points, evaluate their barycenter and use their barycenter as their origin
   vec3 acqP[3], worldP[3];
   vec3 acqCenter = vec3(0.f, 0.f, 0.f), worldCenter = vec3(0.f, 0.f, 0.f);
   vec3 defaults[] = { vec3(CMTOVPU(0), CMTOVPU(30), CMTOVPU(80)), vec3(CMTOVPU(-15), CMTOVPU(10), CMTOVPU(5)), vec3(CMTOVPU(15), CMTOVPU(10), CMTOVPU(5)) };
   for (int i = 0; i < 3; i++)
   {
      acqP[i].x = LoadValueWithDefault(regKey[RegName::Headtracking], "HeadtrackerP"s.append(std::to_string(i + 1)).append("X"s), defaults[i].x);
      acqP[i].y = LoadValueWithDefault(regKey[RegName::Headtracking], "HeadtrackerP"s.append(std::to_string(i + 1)).append("Y"s), defaults[i].y);
      acqP[i].z = LoadValueWithDefault(regKey[RegName::Headtracking], "HeadtrackerP"s.append(std::to_string(i + 1)).append("Z"s), defaults[i].z);
      acqCenter = acqCenter + (acqP[i] / 3.f);
      worldP[i].x = LoadValueWithDefault(regKey[RegName::Headtracking], "PlayerP"s.append(std::to_string(i + 1)).append("X"s), defaults[i].x);
      worldP[i].y = LoadValueWithDefault(regKey[RegName::Headtracking], "PlayerP"s.append(std::to_string(i + 1)).append("Y"s), defaults[i].y);
      worldP[i].z = LoadValueWithDefault(regKey[RegName::Headtracking], "PlayerP"s.append(std::to_string(i + 1)).append("Z"s), defaults[i].z);
      worldCenter = worldCenter + (worldP[i] / 3.f);
   }
   float acqLengthAvg = 0.f;
   float worldLengthAvg = 0.f;
   for (int i = 0; i < 3; i++)
   {
      acqP[i] = acqP[i] - acqCenter;
      worldP[i] = worldP[i] - worldCenter;
      acqLengthAvg += sqrt(acqP[i].x * acqP[i].x + acqP[i].y * acqP[i].y + acqP[i].z * acqP[i].z);
      worldLengthAvg += sqrt(worldP[i].x * worldP[i].x + worldP[i].y * worldP[i].y + worldP[i].z * worldP[i].z);
   }
   m_scale = worldLengthAvg / acqLengthAvg;

   // Perform SVD decomposition to get rotation and translation
   Matrix3 acqMat, worldMatT;
   for (int i = 0; i < 3; i++)
   {
      acqMat.m_d[i][0] = acqP[i].x;
      acqMat.m_d[i][1] = acqP[i].y;
      acqMat.m_d[i][2] = acqP[i].z;
      worldMatT.m_d[0][i] = worldP[i].x;
      worldMatT.m_d[1][i] = worldP[i].y;
      worldMatT.m_d[2][i] = worldP[i].z;
   }
   Matrix3 h = acqMat * worldMatT;

   Eigen::Matrix3f m;
   for (int i = 0; i < 3; i++)
      for (int j = 0; j < 3; j++)
         m(i, j) = h.m_d[i][j];
   Eigen::JacobiSVD svd(m, Eigen::ComputeFullU | Eigen::ComputeFullV);
   m_rotation = svd.matrixV() * (svd.matrixU().transpose());

   Eigen::Vector3f acqCentroid(acqCenter.x, acqCenter.y, acqCenter.z);
   Eigen::Vector3f worldCentroid(worldCenter.x, worldCenter.y, worldCenter.z);
   m_translation = worldCentroid - m_rotation * m_scale * acqCentroid;

   if (0)
   {
      std::stringstream ss;
      ss << "\n";
      ss << "rotation:\n" << m_rotation << "\n\n";
      ss << "translation:\n" << m_translation << "\n\n";
      ss << "scale: " <<m_scale << "\n\n";
      for (int i = 0; i < 3; i++)
      {
         acqP[i].x = LoadValueWithDefault(regKey[RegName::Headtracking], "HeadtrackerP"s.append(std::to_string(i + 1)).append("X"s), defaults[i].x);
         acqP[i].y = LoadValueWithDefault(regKey[RegName::Headtracking], "HeadtrackerP"s.append(std::to_string(i + 1)).append("Y"s), defaults[i].y);
         acqP[i].z = LoadValueWithDefault(regKey[RegName::Headtracking], "HeadtrackerP"s.append(std::to_string(i + 1)).append("Z"s), defaults[i].z);
         worldP[i].x = LoadValueWithDefault(regKey[RegName::Headtracking], "PlayerP"s.append(std::to_string(i + 1)).append("X"s), defaults[i].x);
         worldP[i].y = LoadValueWithDefault(regKey[RegName::Headtracking], "PlayerP"s.append(std::to_string(i + 1)).append("Y"s), defaults[i].y);
         worldP[i].z = LoadValueWithDefault(regKey[RegName::Headtracking], "PlayerP"s.append(std::to_string(i + 1)).append("Z"s), defaults[i].z);
         Eigen::Vector3f acqPos(acqP[i].x, acqP[i].y, acqP[i].z);
         ss << "\nAcquired position:\n" << acqPos;
         ss << "\n> Reference:\n" << Eigen::Vector3f(worldP[i].x, worldP[i].y, worldP[i].z);
         Eigen::Vector3f worldPos = m_rotation * m_scale * acqPos + m_translation;
         ss << "\n> Transformed to:\n" << worldPos;
         ss << "\n";
      }
      OutputDebugString(ss.str().c_str());
   }
}

void HeadTracker::GetLastAcquired(vec3& position) const
{
   position = m_lastAcquiredPos;
}


///////////////////////////////////////////////////////////////////
// UDP (Opentrack UDP communication for Window, Mac and Linux)
#ifdef HEADTRACKER_UDP
bool HeadTracker::UpdateUDP()
{
   // TODO implement
   return false;
}

void HeadTracker::ReleaseUDP()
{
   // TODO implement
}
#endif


///////////////////////////////////////////////////////////////////
// BAM (Windows mapped memory communication)
#ifdef HEADTRACKER_BAM
#define BAM_SHAREDMEM_FILENAME "BAM-Tracker-Shared-Memory"

bool HeadTracker::UpdateBAM()
{
   if (m_BAMData == nullptr)
   {
      m_BAMMemMap = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(FTHeap), (LPCSTR)BAM_SHAREDMEM_FILENAME);
      if (m_BAMMemMap == nullptr)
      {
         m_BAMData = nullptr;
         return false;
      }
      m_BAMData = (BAMData*)MapViewOfFile(m_BAMMemMap, FILE_MAP_READ, 0, 0, sizeof(BAMData));
      if (m_BAMData == nullptr)
      {
         CloseHandle(m_BAMMemMap);
         m_BAMMemMap = NULL;
         return false;
      }
   }
   if (m_BAMData->ScreenWidth == 0 || m_BAMData->ScreenHeight == 0)
      return false;
   BAMPlayerData d = m_BAMData->Data[m_BAMData->UsedDataSlot];
   m_lastAcquiredPos.x = (float)d.EndPosition[0];
   m_lastAcquiredPos.y = (float)d.EndPosition[1];
   m_lastAcquiredPos.z = (float)d.EndPosition[2];
   return true;
}

void HeadTracker::ReleaseBAM()
{
   if (m_BAMData != nullptr)
      UnmapViewOfFile(m_BAMData);
   if (m_BAMMemMap != nullptr)
      CloseHandle(m_BAMMemMap);
}
#endif


///////////////////////////////////////////////////////////////////
// FreeTracker 2 (Windows mapped memory communication)
// 
// Implementation adapted from OpenTrack: https://github.com/opentrack/opentrack/tree/unstable/freetrackclient
//
#ifdef HEADTRACKER_FREETRACKER2
#define FREETRACK_SHAREDMEM_FILENAME "FT_SharedMem"
#define FREETRACK_MUTEX "FT_Mutext"

bool HeadTracker::UpdateFreeTracker()
{
   if (m_FTData == nullptr)
   {
      m_FTMemMap = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(FTHeap), (LPCSTR)FREETRACK_SHAREDMEM_FILENAME);
      if (m_FTMemMap == nullptr)
      {
         m_FTData = nullptr;
         return false;
      }
      m_FTData = (FTHeap*)MapViewOfFile(m_FTMemMap, FILE_MAP_WRITE, 0, 0, sizeof(FTHeap));
      if (m_FTData == nullptr)
      {
         CloseHandle(m_FTMemMap);
         m_FTMemMap = NULL;
         return false;
      }
      m_FTMutex = CreateMutexA(nullptr, FALSE, FREETRACK_MUTEX);
   }
   if (m_FTMutex && WaitForSingleObject(m_FTMutex, 16) == WAIT_OBJECT_0)
   {
      if (m_FTData->data.DataID > (1 << 29))
         m_FTData->data.DataID = 0;
      m_lastAcquiredPos.x = m_FTData->data.X;
      m_lastAcquiredPos.y = m_FTData->data.Y;
      m_lastAcquiredPos.z = m_FTData->data.Z;
      ReleaseMutex(m_FTMutex);
   }
   return true;
}

void HeadTracker::ReleaseFreeTracker()
{
   if (m_FTMemMap != nullptr)
      CloseHandle(m_FTMemMap);
   if (m_FTData != nullptr)
      UnmapViewOfFile((LPVOID)m_FTData);
   if (m_FTMutex != nullptr)
      CloseHandle(m_FTMutex);
}
#endif

