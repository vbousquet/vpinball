// license:GPLv3+

#include "core/stdafx.h"
#include "../vpx-test.h"

#include "parts/flasher.h"
#include "parts/light.h"
#include "parts/pintable.h"
#include "parts/primitive.h"
#include "renderer/Renderer.h"
#include "plugins/MsgPluginManager.h"

#include "doctest.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <tuple>
#include <vector>
#include <set>

// T11 lamp and flasher glints (sphere light specular + Traced insert proxy lights) on the Police Force PBR pack. Local
// tool, runs only when its environment variables are set. VPX_T11=0|1 switches the feature (read by the renderer).
//
//   PF_PACK_DIR=<pack> T11_SHOT=<out.png> [T11_VIEW=main|wire] [T11_ORBIT=<frames>] [T11_ARC=<deg>] [T11_FLASH_I=<intensity>]
//   [T11_CAM="x,y,z,azimuth,elevation,distance,fov"] [PF_HDRI=dancing_hall] [PF_ENV_SCALE=2]
//
// 'main' is the table's own desktop view, 'wire' a free camera close-up of the left wire ramp with flasher f125 fired.
// T11_ORBIT=N captures N frames sweeping the close-up camera over T11_ARC degrees of azimuth (<out>_000.png ...).

static std::string T11Env(const char* name)
{
   const char* const v = std::getenv(name);
   return v ? std::string(v) : std::string();
}

static float T11EnvF(const char* name, float def) { return T11Env(name).empty() ? def : (float)atof(T11Env(name).c_str()); }

TEST_CASE("PF T11 glints [pf][render]")
{
#if defined(ENABLE_BGFX)
   const std::string pack = T11Env("PF_PACK_DIR"), shot = T11Env("T11_SHOT");
   if (pack.empty() || shot.empty())
      return;
   ResetVPX();
   const std::string view = T11Env("T11_VIEW").empty() ? "main" : T11Env("T11_VIEW");
   const std::string hdri = T11Env("PF_HDRI").empty() ? "dancing_hall" : T11Env("PF_HDRI");
   g_settingsService.GetAppSettings().SetPlayer_PlayfieldWidth(1920, false);
   g_settingsService.GetAppSettings().SetPlayer_PlayfieldHeight(1080, false);
   g_settingsService.GetAppSettings().SetPlayer_DisableAO(true, false);
   g_settingsService.GetAppSettings().SetPlayer_SSRefl(false, false);
   g_settingsService.GetAppSettings().SetPlayer_AAFactor(1.f, false);
   g_settingsService.GetAppSettings().SetPlayer_FXAA(6, false);

   CComObject<PinTable>* table;
   CComObject<PinTable>::CreateInstance(&table);
   table->AddRef();
   TestFileFeedback feedback;
   REQUIRE(SUCCEEDED(table->LoadGameFromFilename(std::filesystem::path(pack), feedback)));

   // Same scene as 'PF first look render' (no script, PBR parts only, GI on), all Traced inserts lit and the flashers
   // around the left wire ramp fired as PBR sphere lights (#906 bulb, 0.25in radius)
   table->m_script_text.clear();
   const std::set<std::string> flashers = { "f125", "f126" };
   for (IEditable* part : table->GetParts())
   {
      switch (part->GetItemType())
      {
      case eItemFlasher: static_cast<Flasher*>(part)->m_d.m_isVisible = false; break;
      case eItemPrimitive:
      {
         Primitive* const prim = static_cast<Primitive*>(part);
         const std::string n = prim->GetName();
         if (n.rfind("BM_", 0) != 0 && n.rfind("PBR_", 0) != 0 && n.rfind("TC_", 0) != 0 && n != "playfield_mesh")
            prim->m_d.m_visible = false;
         break;
      }
      case eItemLight:
      {
         Light* const light = static_cast<Light*>(part);
         const std::string n = light->GetName();
         light->m_d.m_fadeSpeedUp = light->m_d.m_fadeSpeedDown = 1000.f;
         if (flashers.count(n))
         {
            light->m_d.m_type = LightType::PBR;
            light->m_d.m_visible = true;
            light->m_d.m_showBulbMesh = false;
            light->m_d.m_color = RGB(255, 236, 214);
            light->m_d.m_intensity = T11EnvF("T11_FLASH_I", 1.f);
            light->m_d.m_falloff = 500.f;
            light->m_d.m_falloff_power = 2.f;
            light->m_d.m_fader = FADER_NONE;
            light->m_d.m_shadows = ShadowMode::NONE;
            light->m_d.m_bulbRadius = 0.25f * 47.06f;
            light->m_d.m_state = light->m_inPlayState = 1.f;
         }
         else if (light->m_d.m_type == LightType::PBR)
            light->m_d.m_state = light->m_inPlayState = 1.f; // GI
         else
            light->m_d.m_visible = false;
         if (n.size() > 1 && n[0] == 'l' && isdigit((unsigned char)n[1]))
            light->m_d.m_state = light->m_inPlayState = 1.f; // every insert lamp on
         break;
      }
      default: break;
      }
   }
   // The exported PBR_ metal meshes are flat shaded (split vertex per face, ~17 deg between neighbour faces on the wires),
   // which breaks every glint into dashes. T11_SMOOTH=1 (default) welds their normals in memory (pack untouched): the
   // normals of the coincident vertices within 60 deg of each other are averaged
   int smoothed = 0;
   if (T11Env("T11_SMOOTH") != "0")
      for (IEditable* part : table->GetParts())
      {
         if (part->GetItemType() != eItemPrimitive)
            continue;
         Primitive* const prim = static_cast<Primitive*>(part);
         const std::string n = prim->GetName();
         if (n.rfind("PBR_", 0) != 0 || (n.find("Metal") == std::string::npos && n.find("wire") == std::string::npos && n.find("kruiskop") == std::string::npos))
            continue;
         auto& verts = prim->m_mesh.m_vertices;
         std::map<std::tuple<int, int, int>, std::vector<size_t>> groups;
         for (size_t i = 0; i < verts.size(); i++)
            groups[{ (int)lroundf(verts[i].x * 100.f), (int)lroundf(verts[i].y * 100.f), (int)lroundf(verts[i].z * 100.f) }].push_back(i);
         std::vector<Vertex3Ds> normals(verts.size());
         for (const auto& [key, idx] : groups)
            for (const size_t i : idx)
            {
               const Vertex3Ds ni(verts[i].nx, verts[i].ny, verts[i].nz);
               Vertex3Ds sum(0.f, 0.f, 0.f);
               for (const size_t j : idx)
               {
                  const Vertex3Ds nj(verts[j].nx, verts[j].ny, verts[j].nz);
                  if (ni.Dot(nj) > 0.5f)
                     sum += nj;
               }
               sum.Normalize();
               normals[i] = sum;
            }
         for (size_t i = 0; i < verts.size(); i++)
         {
            verts[i].nx = normals[i].x;
            verts[i].ny = normals[i].y;
            verts[i].nz = normals[i].z;
         }
         smoothed++;
      }
   MESSAGE("T11: smoothed normals on " << smoothed << " PBR_ metal meshes");
   // T11_WIRE_ROUGH=<r>: roughness of the wire ramp steel (pack 0.27) to judge the glints on polished wire
   if (!T11Env("T11_WIRE_ROUGH").empty())
      for (IEditable* part : table->GetParts())
         if (part->GetItemType() == eItemPrimitive && part->GetName() == "PBR_WireRamp_Metal_Steel_with_AO_dirt")
            if (Material* const m = table->GetMaterial(static_cast<Primitive*>(part)->m_d.m_szMaterial))
               m->m_fRoughness = T11EnvF("T11_WIRE_ROUGH", 0.27f);
   table->m_envImage = "env_" + hdri;
   table->m_envEmissionScale = T11EnvF("PF_ENV_SCALE", 2.f);
   table->m_enableAO = false;
   table->m_enableSSR = false;

   struct ShotState
   {
      std::string path;
      bool freeCam = false;
      float x = 165.f, y = 1180.f, z = 90.f, azimuth = 30.f, elevation = 55.f, distance = 260.f, fov = 30.f, arc = 40.f;
      int orbit = 0, step = 0, settle = 0, frame = 0;
      bool pending = false;
      uint64_t gpuSum = 0;
      int gpuCount = 0;
   } state;
   state.path = shot;
   state.freeCam = view == "wire";
   state.orbit = (int)T11EnvF("T11_ORBIT", 0.f);
   state.arc = T11EnvF("T11_ARC", 40.f);
   if (!T11Env("T11_CAM").empty())
      sscanf(T11Env("T11_CAM").c_str(), "%f,%f,%f,%f,%f,%f,%f", &state.x, &state.y, &state.z, &state.azimuth, &state.elevation, &state.distance, &state.fov);
   msgpi_msg_callback onPrepareFrame = [](const unsigned int msgId, void* context, void* msgData)
   {
      ShotState* const s = reinterpret_cast<ShotState*>(context);
      s->frame++;
      if (s->frame == 1)
         g_pplayer->m_renderer->DisableStaticPrePass(true);
      const int shots = max(s->orbit, 1);
      if (s->step >= shots)
      {
         if (!s->pending)
            g_pplayer->SetCloseState(Player::CS_STOP_PLAY);
         return;
      }
      if (s->freeCam)
      {
         // Azimuth 0 looks up the table from the player's side; the orbit sweeps the arc centred on the base azimuth
         const float az = s->azimuth + (s->orbit > 1 ? s->arc * ((float)s->step / (float)(s->orbit - 1) - 0.5f) : 0.f);
         const float e = ANGTORAD(s->elevation), a = ANGTORAD(az);
         const Vertex3Ds eye(s->x + s->distance * sinf(e) * sinf(a), s->y + s->distance * sinf(e) * cosf(a), s->z + s->distance * cosf(e));
         const Matrix3D RH2LH = Matrix3D::MatrixScale(1.f, 1.f, -1.f);
         const Matrix3D YAxis = Matrix3D::MatrixScale(1.f, -1.f, 1.f);
         const Matrix3D camView = Matrix3D::MatrixLookAtRH(Vertex3Ds(eye.x, eye.y, -eye.z), Vertex3Ds(s->x, s->y, -s->z), Vertex3Ds(0.f, 0.f, -1.f));
         const Matrix3D camProj = Matrix3D::MatrixPerspectiveFovRH(s->fov, 1920.f / 1080.f, 5.f, 50000.f);
         g_pplayer->m_renderer->SetViewProj(RH2LH * camView * YAxis, YAxis * camProj);
      }
      if (s->frame > 60 && s->frame <= 180)
      {
         s->gpuSum += g_pplayer->m_renderer->m_renderDevice->m_lastGPUFrameLength;
         s->gpuCount++;
      }
      if (s->frame < 200 || s->pending || ++s->settle < 3)
         return;
      s->pending = true;
      std::string path = s->path;
      if (s->orbit > 1)
         path = path.substr(0, path.size() - 4) + "_" + std::to_string(1000 + s->step).substr(1) + ".png";
      g_pplayer->m_renderer->m_renderDevice->CaptureScreenshot({ g_pplayer->m_playfieldWnd }, { std::filesystem::path(path) },
         [s](bool)
         {
            s->step++;
            s->settle = 0;
            s->pending = false;
         });
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
   std::ofstream(shot + ".txt") << "gpu_ms=" << (state.gpuCount ? (double)state.gpuSum / state.gpuCount / 1000.0 : -1.0) << " frames=" << state.frame << " shots=" << state.step << '\n';
   ResetVPX();
#endif
}
