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
#include "renderer/RenderProbe.h"
#include "plugins/MsgPluginManager.h"

#include "doctest.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

// Clear coat on the Police Force PBR pack: the playfield, the printed plastics, the flipper bats and the painted black wood
// get a clear coat in memory (the pack is not edited), so their art stays matte under a glossy shell. Same scene as 'PF T10
// plastics' (no script, PBR parts only, the pack's committed look, all GI lights shadowed). The engine gate VPX_COAT=0|1
// switches the coat lobe for A/B stills. Local tool, runs only when its environment variables are set.
//
//   PF_PACK_DIR=<pack> COAT_SHOT=<out.png> [COAT_VIEW=hi|graze|ul] [COAT_CAM="x,y,z,azimuth,elevation,distance,fov"]
//   [COAT_AZ=<azimuth offset, deg>] [COAT_SHADOWS=all|none] [COAT_R=<coat roughness, default 0.03>]
//   [COAT_MATS=0 (keep the pack's materials)] [COAT_PROBE=<playfield reflection probe mode, 5 = dynamic>] [COAT_PF_BASE=0.45] [COAT_PRINT_BASE=0.4] [COAT_BAT=0.8] [COAT_WOOD=0.8]
//
// 'hi' is the table's own desktop view, 'graze' a low grazing view across the lower playfield, 'ul' the upper left plastics.

static std::string CoatEnv(const char* name)
{
   const char* const v = std::getenv(name);
   return v ? std::string(v) : std::string();
}

static float CoatEnvF(const char* name, float def) { return CoatEnv(name).empty() ? def : (float)atof(CoatEnv(name).c_str()); }

TEST_CASE("PF coat [pf][render]")
{
#if defined(ENABLE_BGFX)
   const std::string pack = CoatEnv("PF_PACK_DIR"), shot = CoatEnv("COAT_SHOT");
   if (pack.empty() || shot.empty())
      return;
   ResetVPX();
   const std::string view = CoatEnv("COAT_VIEW").empty() ? "hi" : CoatEnv("COAT_VIEW");
   const std::string shadows = CoatEnv("COAT_SHADOWS").empty() ? "all" : CoatEnv("COAT_SHADOWS");
   const bool gi = CoatEnv("COAT_GI") != "0";
   std::vector<std::string> hide;
   {
      std::stringstream ss(CoatEnv("COAT_HIDE"));
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
               light->m_d.m_state = light->m_inPlayState = gi ? 1.f : 0.f;
               light->m_d.m_shadows = shadows == "all" ? ShadowMode::PBR_STATIC_SHADOWS : ShadowMode::NONE;
            }
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
   // Clear coat per family (the pack's thin transmission is kept): coat 1 on the playfield and printed plastics with a
   // matte base under it, a partial coat on the flipper bats and the painted black wood
   const float coatR = CoatEnvF("COAT_R", 0.03f);
   for (Material* const m : table->m_materials)
   {
      if (CoatEnv("COAT_MATS") == "0") // the pack's own materials (the 'before' still)
         break;
      const std::string& n = m->m_name;
      if (m->m_type != Material::MaterialType::PBR)
         continue;
      if (n == "PF_Playfield")
      {
         m->m_fClearcoat = 1.f;
         m->m_fRoughness = CoatEnvF("COAT_PF_BASE", 0.45f) / 0.384f; // the ORM map's roughness is ~0.384 (median)
      }
      else if (n.find("Plastic_Print") != std::string::npos)
      {
         m->m_fClearcoat = 1.f;
         m->m_fRoughness = CoatEnvF("COAT_PRINT_BASE", 0.4f);
      }
      else if (n.find("Bat_Plastic") != std::string::npos)
         m->m_fClearcoat = CoatEnvF("COAT_BAT", 0.8f);
      else if (n.find("Wood_Painted_Black") != std::string::npos)
         m->m_fClearcoat = CoatEnvF("COAT_WOOD", 0.8f);
      else
         continue;
      m->m_fClearcoatRoughness = coatR;
      dbg << "coat " << n << " " << m->m_fClearcoat << " r " << m->m_fClearcoatRoughness << " base r " << m->m_fRoughness << "\n";
   }
   // Optional planar playfield reflections (the pack ships them off): the coat then reflects the parts above the playfield
   if (!CoatEnv("COAT_PROBE").empty())
      for (RenderProbe* const probe : table->GetRenderProbeList(RenderProbe::PLANE_REFLECTION))
      {
         probe->SetReflectionMode(static_cast<RenderProbe::ReflectionMode>(atoi(CoatEnv("COAT_PROBE").c_str())));
         dbg << "probe " << probe->GetName() << " mode " << (int)probe->GetReflectionMode() << "\n";
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
   if (view != "hi")
   {
      state.freeCam = true;
      if (view == "graze") { state.x = 476.f; state.y = 1450.f; state.z = 0.f; state.azimuth = 0.f; state.elevation = 80.f; state.distance = 750.f; state.fov = 40.f; }
      else if (view == "ul") { state.x = 230.f; state.y = 960.f; state.z = 30.f; state.azimuth = 20.f; state.elevation = 45.f; state.distance = 420.f; state.fov = 34.f; }
      if (!CoatEnv("COAT_CAM").empty())
         sscanf(CoatEnv("COAT_CAM").c_str(), "%f,%f,%f,%f,%f,%f,%f", &state.x, &state.y, &state.z, &state.azimuth, &state.elevation, &state.distance, &state.fov);
   }
   state.azimuth += CoatEnvF("COAT_AZ", 0.f);
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
