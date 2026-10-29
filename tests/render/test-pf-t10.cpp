// license:GPLv3+

#include "core/stdafx.h"
#include "../vpx-test.h"

#include "parts/flasher.h"
#include "parts/light.h"
#include "parts/pintable.h"
#include "parts/primitive.h"
#include "parts/surface.h"
#include "parts/Material.h"
#include "renderer/Renderer.h"
#include "plugins/MsgPluginManager.h"

#include "doctest.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

// T10 GI light through the playfield plastics on the Police Force PBR pack (GI shadows, thin translucency, coloured
// transmittance shadows). Local tool, runs only when its environment variables are set. Same scene as 'PF first look
// render' (no script, PBR parts only, the pack's committed look: env, room keys, tonemapper, exposure), edits in memory.
//
//   PF_PACK_DIR=<pack> T10_SHOT=<out.png> [T10_VIEW=hi|lower|low|top|sling|ul] [T10_CAM="x,y,z,azimuth,elevation,distance,fov"]
//   [T10_GI=1|0] [T10_GI_ONLY=<gi name>] [T10_SHADOWS=none|all|<gi name>] [T10_GI_H=<height of the single shadowed GI>]
//   [T10_ROOM_SHADOWS=0] [T10_HIDE=<name substrings, comma separated>] [T10_POOL=<shadow pool pages>] [T10_SHADOW_RES=<ortho texels>]
//   [T10_TRANS=<plastics transmission scale, 0 = off>] [T10_T_PRINT=0.12] [T10_T_LANE=0.15] [T10_T_COLOR=0.35]
//
// 'hi' is the table's own desktop view, 'lower'/'low' the pf-pbr still cameras (t10_shots.sh), the others free cameras:
// 'top' looks straight down on the lower playfield, 'sling' and 'ul' are close-ups of the left sling and upper left plastics.

static std::string T10Env(const char* name)
{
   const char* const v = std::getenv(name);
   return v ? std::string(v) : std::string();
}

static float T10EnvF(const char* name, float def) { return T10Env(name).empty() ? def : (float)atof(T10Env(name).c_str()); }

TEST_CASE("PF T10 plastics [pf][render]")
{
#if defined(ENABLE_BGFX)
   const std::string pack = T10Env("PF_PACK_DIR"), shot = T10Env("T10_SHOT");
   if (pack.empty() || shot.empty())
      return;
   ResetVPX();
   const std::string view = T10Env("T10_VIEW").empty() ? "hi" : T10Env("T10_VIEW");
   const std::string shadows = T10Env("T10_SHADOWS").empty() ? "none" : T10Env("T10_SHADOWS");
   const bool gi = T10Env("T10_GI") != "0";
   std::vector<std::string> hide;
   {
      std::stringstream ss(T10Env("T10_HIDE"));
      for (std::string item; std::getline(ss, item, ',');)
         if (!item.empty())
            hide.push_back(item);
   }
   {
      Settings& settings = g_settingsService.GetAppSettings();
      settings.SetPlayer_PlayfieldWidth(1920, false);
      settings.SetPlayer_PlayfieldHeight(1080, false);
      settings.SetPlayer_DisableAO(true, false);
      settings.SetPlayer_SSRefl(false, false);
      settings.SetPlayer_AAFactor(1.f, false);
      settings.SetPlayer_FXAA(6, false);
      if (!T10Env("T10_SHADOW_RES").empty())
         settings.SetPlayer_PBRShadowMapSize(atoi(T10Env("T10_SHADOW_RES").c_str()), false);
      if (!T10Env("T10_POOL").empty())
         settings.SetPlayer_PBRShadowPoolPages(atoi(T10Env("T10_POOL").c_str()), false);
   }

   CComObject<PinTable>* table;
   CComObject<PinTable>::CreateInstance(&table);
   table->AddRef();
   TestFileFeedback feedback;
   REQUIRE(SUCCEEDED(table->LoadGameFromFilename(std::filesystem::path(pack), feedback)));

   table->m_script_text.clear();
   static const int roundLamps[] = { 14, 24, 25, 26, 27, 28, 29, 30, 49, 50, 51, 52, 54, 55, 56, 57, 58, 59 };
   std::set<int> lampsOn;
   for (size_t i = 0; i < std::size(roundLamps); i += 2)
      lampsOn.insert(roundLamps[i]);
   std::ofstream dbg(shot + ".dbg");
   for (IEditable* part : table->GetParts())
   {
      const std::string n = part->GetName();
      bool hidden = false;
      for (const std::string& h : hide)
         hidden |= n.find(h) != std::string::npos;
      switch (part->GetItemType())
      {
      case eItemFlasher: static_cast<Flasher*>(part)->m_d.m_isVisible = false; break;
      case eItemPrimitive:
      {
         Primitive* const prim = static_cast<Primitive*>(part);
         if (hidden || (n.rfind("BM_", 0) != 0 && n.rfind("PBR_", 0) != 0 && n.rfind("TC_", 0) != 0 && n != "playfield_mesh"))
            prim->m_d.m_visible = false;
         if (hidden)
            dbg << "hidden " << n << "\n";
         break;
      }
      case eItemSurface:
         if (hidden)
         {
            static_cast<Surface*>(part)->m_d.m_visible = false;
            dbg << "hidden " << n << "\n";
         }
         break;
      case eItemLight:
      {
         Light* const light = static_cast<Light*>(part);
         light->m_d.m_fadeSpeedUp = light->m_d.m_fadeSpeedDown = 1000.f;
         if (light->m_d.m_type == LightType::PBR)
         {
            const bool isGI = n.size() == 4 && n[0] == 'g' && n[1] == 'i';
            if (isGI)
            {
               light->m_d.m_state = light->m_inPlayState = (gi && (T10Env("T10_GI_ONLY").empty() || T10Env("T10_GI_ONLY") == n)) ? 1.f : 0.f;
               light->m_d.m_shadows = (shadows == "all" || shadows == n) ? ShadowMode::PBR_STATIC_SHADOWS : ShadowMode::NONE;
               if (shadows == n && !T10Env("T10_GI_H").empty())
                  light->m_d.m_height = T10EnvF("T10_GI_H", 17.66f);
            }
            else if (T10Env("T10_ROOM_SHADOWS") == "0")
               light->m_d.m_shadows = ShadowMode::NONE; // room keys
         }
         else if (light->m_d.m_visible)
            light->m_d.m_visible = false;
         if (n.size() > 1 && n[0] == 'l' && isdigit((unsigned char)n[1]))
            light->m_d.m_state = light->m_inPlayState = lampsOn.count(atoi(n.c_str() + 1)) ? 1.f : 0.f;
         break;
      }
      default: break;
      }
   }
   // Thin surface transmission (PBR materials: wrap lighting) of the plastics, per family, scaled by T10_TRANS (0 = off)
   const float trans = T10EnvF("T10_TRANS", 1.f);
   for (Material* const m : table->m_materials)
   {
      const std::string& n = m->m_name;
      if (m->m_type != Material::MaterialType::PBR)
         continue;
      float t = 0.f;
      if (n.find("Plastic_Print") != std::string::npos)
         t = T10EnvF("T10_T_PRINT", 0.12f); // printed plastics (white backed ink): the art filters the transmitted light
      else if (n.find("Lane_Plastic") != std::string::npos)
         t = T10EnvF("T10_T_LANE", 0.15f); // opaque coloured lane guides
      else if (n.find("Plastic_Peg") != std::string::npos || n.find("Bumper_Plastic") != std::string::npos)
         t = T10EnvF("T10_T_COLOR", 0.35f); // translucent coloured pegs and bumper caps
      // Clear plastics keep no transmission: nothing to glow in, their opacity alone makes them translucent shadow casters
      if (t > 0.f)
      {
         m->m_fWrapLighting = t * trans;
         dbg << "transmission " << n << " " << m->m_fWrapLighting << "\n";
      }
   }
   table->m_enableAO = false;
   table->m_enableSSR = false;

   struct ShotState
   {
      std::string path;
      bool freeCam = false;
      float x = 476.f, y = 1500.f, z = 0.f, azimuth = 0.f, elevation = 2.f, distance = 1500.f, fov = 36.f;
      int frame = 0;
      bool pending = false, done = false;
      uint64_t gpuSum = 0;
      int gpuCount = 0;
      std::chrono::steady_clock::time_point start;
   } state;
   state.path = shot;
   if (view == "lower" || view == "low")
   {
      // pf-pbr's still cameras (t10_shots.sh): elevation, distance, fov, y fraction of the table length
      const float elevation = view == "lower" ? 14.f : 8.f, distance = view == "lower" ? 800.f : 650.f, fov = view == "lower" ? 34.f : 42.f,
                  yFrac = view == "lower" ? 0.80f : 0.86f;
      const float y = table->m_bottom * yFrac, e = ANGTORAD(elevation);
      for (ViewSetup& vs : table->mViewSetups)
      {
         vs.mMode = VLM_CAMERA;
         vs.mViewX = 0.f;
         vs.mViewY = (y + distance * sinf(e)) - table->m_bottom;
         vs.mViewZ = distance * cosf(e);
         vs.mLookAt = 100.f * (table->m_bottom - y) / table->m_bottom;
         vs.mFOV = fov;
         vs.mLayback = 0.f;
         vs.mViewHOfs = 0.f;
         vs.mViewVOfs = 0.f;
         vs.mViewportRotation = 0.f;
         vs.mSceneScaleX = vs.mSceneScaleY = vs.mSceneScaleZ = 1.f;
      }
   }
   else if (view != "hi")
   {
      state.freeCam = true;
      if (view == "sling") { state.x = 215.f; state.y = 1610.f; state.z = 25.f; state.azimuth = 15.f; state.elevation = 50.f; state.distance = 330.f; state.fov = 30.f; }
      else if (view == "ul") { state.x = 230.f; state.y = 960.f; state.z = 30.f; state.azimuth = 20.f; state.elevation = 45.f; state.distance = 560.f; state.fov = 34.f; }
      if (!T10Env("T10_CAM").empty())
         sscanf(T10Env("T10_CAM").c_str(), "%f,%f,%f,%f,%f,%f,%f", &state.x, &state.y, &state.z, &state.azimuth, &state.elevation, &state.distance, &state.fov);
   }
   msgpi_msg_callback onPrepareFrame = [](const unsigned int msgId, void* context, void* msgData)
   {
      ShotState* const s = reinterpret_cast<ShotState*>(context);
      if (s->frame == 0)
         s->start = std::chrono::steady_clock::now();
      s->frame++;
      if (s->freeCam)
      {
         if (s->frame == 1)
            g_pplayer->m_renderer->DisableStaticPrePass(true);
         const float e = ANGTORAD(s->elevation), a = ANGTORAD(s->azimuth);
         const Vertex3Ds eye(s->x + s->distance * sinf(e) * sinf(a), s->y + s->distance * sinf(e) * cosf(a), s->z + s->distance * cosf(e));
         const Matrix3D RH2LH = Matrix3D::MatrixScale(1.f, 1.f, -1.f);
         const Matrix3D YAxis = Matrix3D::MatrixScale(1.f, -1.f, 1.f);
         const Matrix3D camView = Matrix3D::MatrixLookAtRH(Vertex3Ds(eye.x, eye.y, -eye.z), Vertex3Ds(s->x, s->y, -s->z), Vertex3Ds(0.f, 0.f, -1.f));
         const Matrix3D camProj = Matrix3D::MatrixPerspectiveFovRH(s->fov, 1920.f / 1080.f, 5.f, 50000.f);
         g_pplayer->m_renderer->SetViewProj(RH2LH * camView * YAxis, YAxis * camProj);
      }
      if (s->frame > 90 && s->frame <= 210)
      {
         s->gpuSum += g_pplayer->m_renderer->m_renderDevice->m_lastGPUFrameLength;
         s->gpuCount++;
      }
      if (s->done)
      {
         g_pplayer->SetCloseState(Player::CS_STOP_PLAY);
         return;
      }
      const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - s->start).count();
      if (s->pending || s->frame < 220 || secs < 4.0)
         return;
      s->pending = true;
      g_pplayer->m_renderer->m_renderDevice->CaptureScreenshot({ g_pplayer->m_playfieldWnd }, { std::filesystem::path(s->path) }, [s](bool) { s->done = true; });
   };
   LoadProgress loadProgress;
   auto player = std::make_unique<Player>(table, Player::PlayMode::Play, loadProgress);
   const unsigned int msgId = player->m_pluginManager.GetMsgAPI().GetMsgID(VPXPI_NAMESPACE, VPXPI_EVT_ON_PREPARE_FRAME);
   player->m_pluginManager.GetMsgAPI().SubscribeMsg(player->m_pluginAPI.GetVPXEndPointId(), msgId, onPrepareFrame, &state);
   player->GameLoop();
   player->m_pluginManager.GetMsgAPI().UnsubscribeMsg(msgId, onPrepareFrame, &state);
   player->m_pluginManager.GetMsgAPI().ReleaseMsgID(msgId);
   player = nullptr;
   table->Release();
   std::ofstream(shot + ".txt") << "gpu_ms=" << (state.gpuCount ? (double)state.gpuSum / state.gpuCount / 1000.0 : -1.0) << " frames=" << state.frame << '\n';
   CHECK(std::filesystem::exists(shot));
   ResetVPX();
#endif
}
