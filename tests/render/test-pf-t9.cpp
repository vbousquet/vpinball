// license:GPLv3+

#include "core/stdafx.h"
#include "../vpx-test.h"

#include "parts/flasher.h"
#include "parts/light.h"
#include "parts/pintable.h"
#include "parts/primitive.h"
#include "renderer/Renderer.h"
#include "plugins/MsgPluginManager.h"
#include "utils/bulb.h"

#include "doctest.h"

#include <filesystem>
#include <fstream>
#include <set>
#include <vector>

// T9 flasher domes on the Police Force PBR pack (fs_flasher_dome.sc + one shadowed PBR burst light per dome). Local tool,
// runs only when its environment variables are set; the pack is changed in memory only. VPX_T9=0 switches the dome layer
// off (read by the renderer), VPX_T9_TUNE tunes it.
//
//   PF_PACK_DIR=<pack> T9_SHOT=<out.png> [T9_VIEW=dome|main|pf] [T9_DOME=f125] [T9_ALL=1] [T9_LEVELS=0,0.5,1]
//   [T9_CLIP=<frames>] [T9_CAM="azimuth,elevation,distance,fov"] [T9_CAMS="cam;cam;..."] [T9_LIGHT=0] [T9_FLASH_I=<intensity>]
//   [T9_BODY="r,g,b,opacity,roughness"|pack] [T9_TINT="r,g,b"] [T9_REFR=<refraction probe>] [T9_REFR_THICK=<units>]
//
// The pack's one 'PBR_Parts_Flasher_Red_Gedan' mesh holds the three playfield domes: it is split per dome (triangles go to
// the nearest of the dome lamps f125 shark, f126 croc, f127 rat) into 'DOME_' primitives linked to their lamp, and each
// lamp becomes a shadowed PBR light at its dome's filament (#906, 0.25in bulb radius, red spill through the plastic).
// 'dome' frames a close-up of T9_DOME around its bulb, 'pf' a wider look at the light it throws, 'main' the desktop view.
// T9_LEVELS captures one still per lamp level and camera (<out>_L<level>.png, <out>_c<cam>_L<level>.png). T9_CLIP=N
// captures N frames on a 60 fps virtual clock (<out>_000.png ...) with the lamp fired 3 times (33 ms pulse at 20 V on
// VPX's #906 filament model, 400 ms apart). T9_ALL=1 drives all three dome lamps, else only T9_DOME's.

static std::string T9Env(const char* name)
{
   const char* const v = std::getenv(name);
   return v ? std::string(v) : std::string();
}

static float T9EnvF(const char* name, float def) { return T9Env(name).empty() ? def : (float)atof(T9Env(name).c_str()); }

TEST_CASE("PF T9 flasher domes [pf][render]")
{
#if defined(ENABLE_BGFX)
   const std::string pack = T9Env("PF_PACK_DIR"), shot = T9Env("T9_SHOT");
   if (pack.empty() || shot.empty())
      return;
   ResetVPX();
   const std::string view = T9Env("T9_VIEW").empty() ? "dome" : T9Env("T9_VIEW");
   const std::string domeLamp = T9Env("T9_DOME").empty() ? "f125" : T9Env("T9_DOME");
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

   // Same scene as 'PF T11 glints' (no script, PBR parts only, GI and inserts on)
   table->m_script_text.clear();
   const std::vector<std::string> lamps = { "f125", "f126", "f127" };
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
         if (light->m_d.m_type == LightType::PBR)
            light->m_d.m_state = light->m_inPlayState = 1.f; // GI
         else if (std::find(lamps.begin(), lamps.end(), n) == lamps.end())
            light->m_d.m_visible = false;
         if (n.size() > 1 && n[0] == 'l' && isdigit((unsigned char)n[1]))
            light->m_d.m_state = light->m_inPlayState = 1.f; // every insert lamp on
         break;
      }
      default: break;
      }
   }

   // Split the domes mesh per lamp into 'DOME_' primitives
   Primitive* domes = nullptr;
   for (IEditable* part : table->GetParts())
      if (part->GetItemType() == eItemPrimitive && part->GetName() == "PBR_Parts_Flasher_Red_Gedan")
         domes = static_cast<Primitive*>(part);
   REQUIRE(domes != nullptr);
   domes->m_d.m_visible = false;
   // Dome body material: T9_BODY="r,g,b,opacity,roughness" (sRGB 0..255) overrides the pack's red
   std::string bodyMat = domes->m_d.m_szMaterial;
   if (T9Env("T9_BODY") != "pack")
   {
      float r = 3.f, g = 0.f, b = 0.f, opacity = 1.f, rough = 0.04f;
      sscanf(T9Env("T9_BODY").c_str(), "%f,%f,%f,%f,%f", &r, &g, &b, &opacity, &rough);
      Material* const src = table->GetMaterial(bodyMat);
      Material* const m = new Material(src);
      m->m_name = "T9_DomeBody";
      m->m_cBase = RGB((int)r, (int)g, (int)b);
      m->m_fOpacity = opacity;
      m->m_bOpacityActive = opacity < 1.f;
      m->m_fRoughness = rough;
      int tr = 150, tg = 4, tb = 2;
      if (!T9Env("T9_TINT").empty())
         sscanf(T9Env("T9_TINT").c_str(), "%d,%d,%d", &tr, &tg, &tb);
      m->m_cRefractionTint = RGB(tr, tg, tb);
      table->AddMaterial(m);
      bodyMat = m->m_name;
   }
   std::vector<Vertex3Ds> lampPos;
   for (const std::string& n : lamps)
   {
      Light* const l = table->GetLight(n);
      REQUIRE(l != nullptr);
      lampPos.emplace_back(l->m_d.m_vCenter.x, l->m_d.m_vCenter.y, 0.f);
   }
   const Mesh& src = domes->m_mesh;
   for (size_t li = 0; li < lamps.size(); li++)
   {
      Primitive* const dome = Primitive::COMCreate();
      dome->Init(0.f, 0.f, false);
      dome->SetName("DOME_" + lamps[li]);
      dome->m_mesh.Clear();
      std::vector<int> remap(src.m_vertices.size(), -1);
      for (size_t t = 0; t + 2 < src.m_indices.size(); t += 3)
      {
         Vertex3Ds c(0.f, 0.f, 0.f);
         for (int k = 0; k < 3; k++)
            c += Vertex3Ds(src.m_vertices[src.m_indices[t + k]].x, src.m_vertices[src.m_indices[t + k]].y, 0.f);
         c = c * (1.f / 3.f);
         size_t best = 0;
         for (size_t j = 1; j < lampPos.size(); j++)
            if ((lampPos[j] - c).LengthSquared() < (lampPos[best] - c).LengthSquared())
               best = j;
         if (best != li)
            continue;
         for (int k = 0; k < 3; k++)
         {
            const unsigned int vi = src.m_indices[t + k];
            if (remap[vi] < 0)
            {
               remap[vi] = (int)dome->m_mesh.m_vertices.size();
               dome->m_mesh.m_vertices.push_back(src.m_vertices[vi]);
            }
            dome->m_mesh.m_indices.push_back(remap[vi]);
         }
      }
      dome->m_mesh.middlePoint = Vertex3Ds(0.f, 0.f, 0.f);
      dome->m_mesh.UpdateBounds();
      dome->m_d.m_use3DMesh = true;
      dome->m_d.m_vPosition = domes->m_d.m_vPosition;
      dome->m_d.m_vSize = domes->m_d.m_vSize;
      dome->m_d.m_szMaterial = bodyMat;
      // The body is the outer skin only (the engine draws no inner face): a near black, smooth, opaque skin carrying the
      // outer highlight (a diffuse red body would light up flat under the GI bulbs next to it, unlike clear plastic); the dome layer draws everything seen inside the wall (T9_REFR=<probe> tries a refractive skin)
      dome->m_d.m_szRefractionProbe = T9Env("T9_REFR");
      dome->m_d.m_refractionThickness = T9EnvF("T9_REFR_THICK", 10.f);
      dome->m_d.m_szORMMap = domes->m_d.m_szORMMap;
      dome->m_d.m_szImage = "DOME_red"s;
      if (T9Env("VPX_T9") != "0") // dome layer off (A/B): a plain PBR body, its lamp link would fade it out
         dome->m_d.m_szLightmap = lamps[li];
      dome->m_d.m_staticRendering = false;
      dome->m_d.m_collidable = false;
      dome->m_d.m_toy = true;
      dome->m_d.m_useDepthMask = true;
      table->AddPart(dome);
      dome->Release();
      MESSAGE("T9: DOME_" << lamps[li] << " " << dome->m_mesh.NumIndices() / 3 << " triangles");
      // The lamp: a shadowed PBR sphere light at the dome's filament, red spill through the plastic
      Light* const light = table->GetLight(lamps[li]);
      Vertex3Ds bulb;
      dome->m_d.m_szImage = "DOME_red"s; // measured as a dome even with the layer off (bulb position)
      REQUIRE(dome->GetFlasherDomeBulb(bulb));
      light->m_d.m_type = LightType::PBR;
      light->m_d.m_visible = T9Env("T9_LIGHT") != "0";
      light->m_d.m_showBulbMesh = false;
      light->m_d.m_vCenter = Vertex2D(bulb.x, bulb.y);
      light->m_d.m_height = bulb.z;
      light->m_d.m_szSurface.clear();
      light->m_d.m_color = light->m_d.m_color2 = RGB(255, 3, 1);
      light->m_d.m_intensity = T9EnvF("T9_FLASH_I", 6.f);
      light->m_d.m_intensity_scale = 1.f;
      light->m_d.m_falloff = 900.f;
      light->m_d.m_falloff_power = 2.f;
      light->m_d.m_fader = FADER_INCANDESCENT;
      light->m_d.m_fadeSpeedUp = light->m_d.m_fadeSpeedDown = 1e-6f; // the test drives the filament (virtual clock)
      light->m_d.m_shadows = ShadowMode::PBR_STATIC_SHADOWS;
      light->m_d.m_bulbRadius = 0.25f * 47.06f;
      light->m_d.m_state = light->m_inPlayState = 0.f;
      MESSAGE("T9: " << lamps[li] << " at " << bulb.x << "," << bulb.y << "," << bulb.z);
   }
   table->m_enableAO = false;
   table->m_enableSSR = false;

   struct ShotState
   {
      std::string path, view, lamp;
      std::vector<std::string> driven; // lamps following the level (T9_ALL=1: every dome lamp)
      Vertex3Ds target;
      float azimuth = 0.f, elevation = 50.f, distance = 170.f, fov = 30.f;
      std::vector<float> levels;
      std::vector<vec4> cams; // T9_CAMS="az,el,dist,fov;..." one still set per camera
      int clip = 0, step = 0, settle = 0, frame = 0;
      bool pending = false;
      double T = 293.0;
      uint64_t gpuSum = 0;
      int gpuCount = 0;
      std::ofstream log;
   } state;
   state.path = shot;
   state.view = view;
   state.lamp = domeLamp;
   state.driven = T9Env("T9_ALL") == "1" ? lamps : std::vector<std::string> { domeLamp };
   state.clip = (int)T9EnvF("T9_CLIP", 0.f);
   if (view == "pf")
   {
      state.azimuth = 10.f;
      state.elevation = 40.f;
      state.distance = 520.f;
      state.fov = 40.f;
   }
   if (!T9Env("T9_CAM").empty())
      sscanf(T9Env("T9_CAM").c_str(), "%f,%f,%f,%f", &state.azimuth, &state.elevation, &state.distance, &state.fov);
   state.cams.push_back(vec4(state.azimuth, state.elevation, state.distance, state.fov));
   if (!T9Env("T9_CAMS").empty())
   {
      state.cams.clear();
      const std::string cs = T9Env("T9_CAMS");
      for (size_t p = 0; p < cs.size();)
      {
         vec4 c;
         sscanf(cs.c_str() + p, "%f,%f,%f,%f", &c.x, &c.y, &c.z, &c.w);
         state.cams.push_back(c);
         const size_t n = cs.find(';', p);
         p = n == std::string::npos ? cs.size() : n + 1;
      }
   }
   {
      const std::string lv = T9Env("T9_LEVELS").empty() ? "0" : T9Env("T9_LEVELS");
      for (size_t p = 0; p < lv.size();)
      {
         state.levels.push_back((float)atof(lv.c_str() + p));
         const size_t c = lv.find(',', p);
         p = c == std::string::npos ? lv.size() : c + 1;
      }
   }
   {
      Light* const l = table->GetLight(domeLamp);
      REQUIRE(l != nullptr);
      state.target = Vertex3Ds(l->m_d.m_vCenter.x, l->m_d.m_vCenter.y, l->m_d.m_height);
   }
   state.log.open(shot + ".log");
   msgpi_msg_callback onPrepareFrame = [](const unsigned int msgId, void* context, void* msgData)
   {
      ShotState* const s = reinterpret_cast<ShotState*>(context);
      s->frame++;
      if (s->frame == 1)
         g_pplayer->m_renderer->DisableStaticPrePass(true);
      const int shots = s->clip > 0 ? s->clip : (int)(s->levels.size() * s->cams.size());
      const int camIdx = s->clip > 0 ? 0 : min(s->step / (int)s->levels.size(), (int)s->cams.size() - 1);
      s->azimuth = s->cams[camIdx].x;
      s->elevation = s->cams[camIdx].y;
      s->distance = s->cams[camIdx].z;
      s->fov = s->cams[camIdx].w;
      if (s->step >= shots)
      {
         if (!s->pending)
            g_pplayer->SetCloseState(Player::CS_STOP_PLAY);
         return;
      }
      if (s->view != "main")
      {
         const float e = ANGTORAD(s->elevation), a = ANGTORAD(s->azimuth);
         const Vertex3Ds& t = s->target;
         const Vertex3Ds eye(t.x + s->distance * sinf(e) * sinf(a), t.y + s->distance * sinf(e) * cosf(a), t.z + s->distance * cosf(e));
         const Matrix3D RH2LH = Matrix3D::MatrixScale(1.f, 1.f, -1.f);
         const Matrix3D YAxis = Matrix3D::MatrixScale(1.f, -1.f, 1.f);
         const Matrix3D camView = Matrix3D::MatrixLookAtRH(Vertex3Ds(eye.x, eye.y, -eye.z), Vertex3Ds(t.x, t.y, -t.z), Vertex3Ds(0.f, 0.f, -1.f));
         const Matrix3D camProj = Matrix3D::MatrixPerspectiveFovRH(s->fov, 1920.f / 1080.f, 5.f, 50000.f);
         g_pplayer->m_renderer->SetViewProj(RH2LH * camView * YAxis, YAxis * camProj);
      }
      // Lamp level: a fixed level per still, or VPX's #906 filament on the virtual clock for the clip
      Light* const lamp = g_pplayer->m_ptable->GetLight(s->lamp);
      const float scale = lamp->m_d.m_intensity * lamp->m_d.m_intensity_scale;
      float level;
      if (s->clip > 0)
         level = bulb_filament_temperature_to_emission(BULB_906, (float)s->T);
      else
      {
         level = s->levels[s->step % s->levels.size()];
         s->T = level > 0.f ? bulb_emission_to_filament_temperature(level) : 293.0;
      }
      for (const std::string& n : s->driven)
      {
         Light* const l = g_pplayer->m_ptable->GetLight(n);
         l->m_currentFilamentTemperature = s->T;
         l->m_currentIntensity = level * scale;
      }
      if (s->frame > 60 && s->frame <= 180)
      {
         s->gpuSum += g_pplayer->m_renderer->m_renderDevice->m_lastGPUFrameLength;
         s->gpuCount++;
      }
      if (s->clip > 0 && s->frame > 200)
         s->log << "frame " << s->frame << " step " << s->step << " gpu_us " << g_pplayer->m_renderer->m_renderDevice->m_lastGPUFrameLength << '\n';
      if (s->frame < 200 || s->pending || ++s->settle < (s->clip > 0 ? 2 : 4))
         return;
      s->pending = true;
      std::string path = s->path.substr(0, s->path.size() - 4);
      if (s->clip > 0)
         path += "_" + std::to_string(1000 + s->step).substr(1) + ".png";
      else
      {
         char buf[32];
         if (s->cams.size() > 1)
            snprintf(buf, sizeof(buf), "_c%d_L%.2f.png", camIdx, level);
         else
            snprintf(buf, sizeof(buf), "_L%.2f.png", level);
         path += buf;
      }
      s->log << "shot " << s->step << " level " << level << " T " << s->T << '\n';
      g_pplayer->m_renderer->m_renderDevice->CaptureScreenshot({ g_pplayer->m_playfieldWnd }, { std::filesystem::path(path) },
         [s](bool)
         {
            if (s->clip > 0)
            {
               // 60 fps virtual clock: 3 pulses of 33 ms at 20 V (WPC flasher drive on a 13 V #906), 400 ms apart
               const int t = s->step * 1000 / 60 - 100;
               const bool on = t >= 0 && t < 1200 && t % 400 < 33;
               s->T = on ? bulb_heat_up(BULB_906, s->T, 1.f / 60.f, 20.f, 0.f) : bulb_cool_down(BULB_906, s->T, 1.f / 60.f);
            }
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
