#include "core/stdafx.h"
#include "LightUIPart.h"

#include "../BlueprintRenderContext.h"
#include "math/matrix.h"
#include "utils/bulb.h"
#include "utils/color.h"


namespace VPX::EditorUI
{

LightUIPart::LightUIPart(Light* light)
   : EditableUIPart(light, { &Data::m_visible })
{
}

LightUIPart::TransformMask LightUIPart::GetTransform(Matrix3D& transform)
{
   // Backdrop lights are flat, in the 2D backdrop XY plane
   if (m_part->m_desktopBackdrop)
      return GetCurveTransform(transform, 0.f);
   const float height = m_part->GetPTable()->GetSurfaceHeight(m_part->m_d.m_szSurface, m_part->m_d.m_vCenter.x, m_part->m_d.m_vCenter.y);
   // PBR lights are point lights: the transform is placed at the bulb position and is translation only
   if (m_part->m_d.m_type == LightType::PBR)
   {
      transform = Matrix3D::MatrixTranslate(m_part->m_d.m_vCenter.x, m_part->m_d.m_vCenter.y, height + m_part->m_d.m_height);
      return TM_TransAny;
   }
   return GetCurveTransform(transform, height + m_part->m_d.m_height);
}

void LightUIPart::SetTransform(const vec3& pos, const vec3& scale, const vec3& rot)
{
   if (!m_part->m_desktopBackdrop)
   {
      const float pz = pos.z - m_part->GetPTable()->GetSurfaceHeight(m_part->m_d.m_szSurface, pos.x, pos.y);
      m_part->m_d.m_bulbHaloHeight = m_part->m_d.m_bulbHaloHeight + (pz - m_part->m_d.m_height);
      m_part->m_d.m_height = pz;
   }
   // PBR lights have no outline curve: the transform only moves the bulb (the curve is kept in sync for consistency)
   if (m_part->m_d.m_type == LightType::PBR)
   {
      m_part->Translate(Vertex2D(pos.x - m_part->m_d.m_vCenter.x, pos.y - m_part->m_d.m_vCenter.y));
      return;
   }
   SetCurveTransform(pos, scale, rot);
}

void LightUIPart::RenderOverlay(const EditorRenderContext& ctx)
{
   // PBR lights have no rendered geometry nor range: only a camera-facing light symbol is drawn at the light position
   if (m_part->m_d.m_type == LightType::PBR)
   {
      // Light symbol at the light position: a small circle with rays around it
      const ImU32 color = ctx.GetColor(ctx.IsSelected());
      const Vertex2D& c = m_part->m_d.m_vCenter;
      const float z = m_part->GetCurrentHeight();
      const ImVec2 cp = ctx.Project(Vertex3Ds(c.x, c.y, z));
      ImDrawList* const drawList = ctx.GetDrawList();
      if (cp.x != FLT_MAX && drawList != nullptr)
      {
         constexpr float iconRadius = 10.f;
         drawList->AddCircle(cp, iconRadius, color);
         constexpr float kDiag = 0.70710678f; // 1/sqrt(2)
         const ImVec2 dirs[8] = { { 1.f, 0.f }, { kDiag, kDiag }, { 0.f, 1.f }, { -kDiag, kDiag }, { -1.f, 0.f }, { -kDiag, -kDiag }, { 0.f, -1.f }, { kDiag, -kDiag } };
         for (const ImVec2& dir : dirs)
            drawList->AddLine(
               ImVec2(cp.x + dir.x * (iconRadius + 3.f), cp.y + dir.y * (iconRadius + 3.f)), ImVec2(cp.x + dir.x * (iconRadius + 9.f), cp.y + dir.y * (iconRadius + 9.f)), color);
      }
      return;
   }

   ctx.DrawWireframe(m_part);

   // Bulb center overlay, same as the win32 editor: falloff ellipse, center crosshair, bulb mesh circle
   const ImU32 color = ctx.GetColor(ctx.IsSelected());
   const Vertex3Ds xAxis(1.f, 0.f, 0.f);
   const Vertex3Ds yAxis(0.f, 1.f, 0.f);
   const Vertex2D& c = m_part->m_d.m_vCenter;
   const float z = m_part->m_surfaceHeight;
   ctx.DrawCircle(Vertex3Ds(c.x, c.y, z), xAxis, yAxis, m_part->m_d.m_falloff, color);
   ctx.DrawLine(Vertex3Ds(c.x - 10.f, c.y, z), Vertex3Ds(c.x + 10.f, c.y, z), color);
   ctx.DrawLine(Vertex3Ds(c.x, c.y - 10.f, z), Vertex3Ds(c.x, c.y + 10.f, z), color);
   if (m_part->m_d.m_showBulbMesh)
      ctx.DrawCircle(Vertex3Ds(c.x, c.y, z), xAxis, yAxis, m_part->m_d.m_meshRadius * 0.5f, color);
}

void LightUIPart::RenderBlueprint(const BlueprintRenderContext& ctx)
{
   if (ctx.GetPass() == BlueprintRenderContext::Pass::Fill)
   {
      ctx.SetBorderColor(-1, false, 0);
      ctx.SetFillColor(m_part->GetPTable()->RenderSolid() ? (((m_part->m_d.m_color & 0xFEFEFE) + (m_part->m_d.m_color2 & 0xFEFEFE)) / 2) : -1);
      ctx.SetObject(this);

      switch (m_part->m_d.m_shape)
      {
      default:
      case ShapeCustom:
         vector<RenderVertex> vvertex;
         m_part->m_curve.GetRgVertex(vvertex);

         // Check if we should display the image in the editor.
         ctx.Polygon(vvertex);

         break;
      }
   }
   else
   {
      bool drawDragpoints = (ctx.m_partSelected || (this == ctx.m_pointEditPart) || g_settingsService.GetAppSettings().GetEditor_ShowDragPoints());

      // if the item is selected then draw the dragpoints (or if we are always to draw dragpoints)
      if (!drawDragpoints)
      {
         // if any of the dragpoints of this object are selected then draw all the dragpoints
         for (const auto& pdp : m_part->m_curve.GetPoints())
         {
            if (ctx.IsPointSelected(pdp.get()))
            {
               drawDragpoints = true;
               break;
            }
         }
      }

      // Port of the win32 RenderOutline helper
      ctx.SetBorderColor(RGB(0, 0, 0), false, 0);
      ctx.SetLineColor(RGB(0, 0, 0), false, 0);
      ctx.SetFillColor(-1);
      ctx.SetObject(this);
      ctx.SetObject(nullptr);

      switch (m_part->m_d.m_shape)
      {
      case ShapeCircle:
      default:
      {
         ctx.Ellipse(m_part->m_d.m_vCenter.x, m_part->m_d.m_vCenter.y, m_part->m_d.m_falloff);
         break;
      }

      case ShapeCustom:
      {
         vector<RenderVertex> vvertex;
         m_part->m_curve.GetRgVertex(vvertex);
         ctx.SetBorderColor(RGB(255, 0, 0), false, 0);
         ctx.Ellipse(m_part->m_d.m_vCenter.x, m_part->m_d.m_vCenter.y, m_part->m_d.m_falloff);
         ctx.SetBorderColor(RGB(0, 0, 0), false, 0);
         ctx.Polygon(vvertex);

         ctx.SetObject(this); // The bulb center is editable as the part's edit center (win32 m_centerPart)
         break;
      }
      }

      if (m_part->m_d.m_shape == ShapeCustom || g_settingsService.GetAppSettings().GetEditor_DrawLightCenters())
      {
         ctx.Line(m_part->m_d.m_vCenter.x - 10.0f, m_part->m_d.m_vCenter.y, m_part->m_d.m_vCenter.x + 10.0f, m_part->m_d.m_vCenter.y);
         ctx.Line(m_part->m_d.m_vCenter.x, m_part->m_d.m_vCenter.y - 10.0f, m_part->m_d.m_vCenter.x, m_part->m_d.m_vCenter.y + 10.0f);
      }

      if (m_part->m_d.m_showBulbMesh)
      {
         ctx.SetBorderColor(RGB(0, 127, 255), false, 0);
         ctx.Ellipse(m_part->m_d.m_vCenter.x, m_part->m_d.m_vCenter.y, m_part->m_d.m_meshRadius * 0.5f);
      }

      if ((m_part->m_d.m_shape == ShapeCustom) && drawDragpoints)
      {
         for (const auto& pdp : m_part->m_curve.GetPoints())
         {
            ctx.SetFillColor(-1);
            ctx.SetBorderColor((ctx.m_hotPoint == pdp.get()) ? RGB(0, 255, 0) : RGB(0, 0, 200), false, 0);
            ctx.SetObject(this, pdp.get());

            ctx.Ellipse2(pdp->GetX(), pdp->GetY(), 8);
         }
      }
   }
}

void LightUIPart::UpdatePropertyPane(PropertyPane& props)
{
   props.EditableHeader("Light"s, m_part);

   UpdateCurveSection(props);
   UpdateCenterSection(props, &Data::m_vCenter);

   if (props.BeginSection("Light Settings"s))
   {
      props.InputFloat<Light>(
         m_part, "Intensity"s, //
         [](const Light* light) { return light->m_d.m_intensity; }, //
         [](Light* light, float v)
         {
            // Fades are stored as speeds (intensity per ms): rescale them so the fade durations stay the same
            if (light->m_d.m_intensity > 0.f && v > 0.f)
            {
               light->m_d.m_fadeSpeedUp *= v / light->m_d.m_intensity;
               light->m_d.m_fadeSpeedDown *= v / light->m_d.m_intensity;
            }
            light->m_d.m_intensity = v;
            light->m_currentIntensity = light->m_d.m_intensity * light->m_d.m_intensity_scale * light->m_d.m_state;
         },
         PropertyPane::Unit::None, 1);
      props.Combo<Light>(
         m_part, "Fader"s, vector<string> { "None"s, "Linear"s, "Incandescent"s }, //
         [](const Light* light) { return static_cast<int>(light->m_d.m_fader); }, //
         [](Light* light, int v) { light->m_d.m_fader = static_cast<Fader>(v); }, "How the light interpolates between states: instantly, linearly, or emulating an incandescent filament");
      props.InputFloat<Light>(
         m_part, "Fade Up (ms)"s, //
         [](const Light* light) { return light->m_d.m_fadeSpeedUp > 1e-6f ? light->m_d.m_intensity * light->m_d.m_intensity_scale / light->m_d.m_fadeSpeedUp : 100000.0f; }, //
         [](Light* light, float v) { light->m_d.m_fadeSpeedUp = v > 0.001f ? light->m_d.m_intensity * light->m_d.m_intensity_scale / v : 100000.0f; }, PropertyPane::Unit::None, 1,
         "Time for the light to ramp from off to its full intensity");
      props.InputFloat<Light>(
         m_part, "Fade Down (ms)"s, //
         [](const Light* light) { return light->m_d.m_fadeSpeedDown > 1e-6f ? light->m_d.m_intensity * light->m_d.m_intensity_scale / light->m_d.m_fadeSpeedDown : 100000.0f; }, //
         [](Light* light, float v) { light->m_d.m_fadeSpeedDown = v > 0.001f ? light->m_d.m_intensity * light->m_d.m_intensity_scale / v : 100000.0f; }, PropertyPane::Unit::None, 1,
         "Time for the light to fade from its full intensity to off");
      props.InputRGB<Light>(
         m_part, "Center Color"s, //
         [](const Light* light) { return convertColor(light->m_d.m_color2); }, //
         [](Light* light, const vec3& v) { light->m_d.m_color2 = convertColorRGB(v); }, "Color at the center of the light shape");
      props.InputRGB<Light>(
         m_part, "Edge Color"s, //
         [](const Light* light) { return convertColor(light->m_d.m_color); }, //
         [](Light* light, const vec3& v) { light->m_d.m_color = convertColorRGB(v); }, "Color at the edge of the light's range");
      props.InputFloat<Light>(
         m_part, "Light Range"s, //
         [](const Light* light) { return light->m_d.m_falloff; }, //
         [](Light* light, float v) { light->m_d.m_falloff = v; }, PropertyPane::Unit::VPLength, 1, "Radius reached by the light on the playfield");
      props.InputFloat<Light>(
         m_part, "Falloff Power"s, //
         [](const Light* light) { return light->m_d.m_falloff_power; }, //
         [](Light* light, float v) { light->m_d.m_falloff_power = v; }, PropertyPane::Unit::None, 1,
         "Exponent of the intensity falloff toward the edge of the light's range (higher = tighter center)");
      props.EndSection();
   }

   if (props.BeginSection("Render Mode"s))
   {
      props.Combo<Light>(
         m_part, "Type"s, vector<string> { "Hidden"s, "Classic"s, "Halo"s, "PBR"s }, //
         [this](const Light* light) { return GetVisibility(light) ? 1 + static_cast<int>(light->m_d.m_type) : 0; },
         [this](Light* light, int v)
         {
            SetVisibility(light, v != 0);
            if (v != 0)
               light->m_d.m_type = static_cast<LightType>(v - 1);
         },
         "Hidden lights the scene without being drawn; Classic draws the flat light shape; Halo renders a 3D bulb; PBR is a physically based light source");
      if (const Light* light = props.GetEditedPart<Light>(m_part); GetVisibility(light))
      {
         props.Checkbox<Light>(
            m_part, "Reflection Enabled"s, //
            [](const Light* light) { return light->m_d.m_reflectionEnabled; }, //
            [](Light* light, bool v) { light->m_d.m_reflectionEnabled = v; }, "Render this part in the table's reflections");
         props.InputFloat<Light>(
            m_part, "Depth Bias"s, //
            [](const Light* light) { return light->m_d.m_depthBias; }, //
            [](Light* light, float v) { light->m_d.m_depthBias = v; }, PropertyPane::Unit::None, 1, "Depth offset applied at render time to fix flickering between overlapping parts");
         if (light->m_d.m_type == LightType::Halo)
         {
            props.InputFloat<Light>(
               m_part, "Halo Height"s, //
               [](const Light* light) { return light->m_d.m_bulbHaloHeight; }, //
               [](Light* light, float v) { light->m_d.m_bulbHaloHeight = v; }, PropertyPane::Unit::VPLength, 1, "Height above the surface at which the bulb halo is drawn");
            props.InputFloat<Light>(
               m_part, "Modulate"s, //
               [](const Light* light) { return light->m_d.m_modulate_vs_add; }, //
               [](Light* light, float v) { light->m_d.m_modulate_vs_add = v; }, PropertyPane::Unit::Percent, 1,
               "Blend between adding light over the surface (0%) and modulating its color (100%)");
            props.InputFloat<Light>(
               m_part, "Transmit"s, //
               [](const Light* light) { return light->m_d.m_transmissionScale; }, //
               [](Light* light, float v) { light->m_d.m_transmissionScale = v; }, PropertyPane::Unit::Percent, 1, "Amount of light transmitted to the back side of the surface below");
         }
         else if (light->m_d.m_type == LightType::Classic)
         {
            props.Checkbox<Light>(
               m_part, "PassThrough"s, //
               [](const Light* light) { return light->m_d.m_imageMode; }, //
               [](Light* light, bool v) { light->m_d.m_imageMode = v; }, "Draw the image unlit instead of rendering it as a lit light");
            props.ImageCombo<Light>(
               m_part, "Image"s, //
               [](const Light* light) { return light->m_d.m_szImage; }, //
               [](Light* light, const string& v) { light->m_d.m_szImage = v; });
         }
         else if (light->m_d.m_type == LightType::PBR)
         {
            props.InputFloat<Light>(
               m_part, "Source Radius"s, //
               [](const Light* light) { return light->m_d.m_bulbRadius; }, //
               [](Light* light, float v) { light->m_d.m_bulbRadius = v; }, PropertyPane::Unit::VPLength, 1);
            props.Checkbox<Light>(
               m_part, "Room Light"s, //
               [](const Light* light) { return light->m_d.m_roomLight; }, //
               [](Light* light, bool v) { light->m_d.m_roomLight = v; });
         }
      }
      props.EndSection();
   }

   if (props.BeginSection("Bulb"s))
   {
      props.Checkbox<Light>(
         m_part, "Show Bulb"s, //
         [](const Light* light) { return light->m_d.m_showBulbMesh; }, //
         [](Light* light, bool v) { light->m_d.m_showBulbMesh = v; });
      props.Checkbox<Light>(
         m_part, "Static Mesh"s, //
         [](const Light* light) { return light->m_d.m_staticBulbMesh; }, //
         [](Light* light, bool v) { light->m_d.m_staticBulbMesh = v; }, "Bake the bulb mesh into the static render pass (faster, but it cannot change at runtime)");
      props.InputFloat<Light>(
         m_part, "Radius"s, //
         [](const Light* light) { return light->m_d.m_meshRadius; }, //
         [](Light* light, float v) { light->m_d.m_meshRadius = v; }, PropertyPane::Unit::VPLength, 1);
      props.EndSection();
   }

   if (props.BeginSection("Ball Reflections & Shadows"s))
   {
      props.Checkbox<Light>(
         m_part, "Show Reflection on Balls"s, //
         [](const Light* light) { return light->m_d.m_showReflectionOnBall; }, //
         [](Light* light, bool v) { light->m_d.m_showReflectionOnBall = v; });
      props.Combo<Light>(
         m_part, "Shadow Mode"s, //
         vector { "None"s, "Raytraced Ball"s, "PBR Static"s, "PBR Static + Ball"s, "PBR Static + Dynamic"s }, //
         [](const Light* light) { return static_cast<int>(light->m_d.m_shadows); }, //
         [](Light* light, int v) { light->m_d.m_shadows = static_cast<ShadowMode>(v); }, "Shadow casting mode of this light");
      props.EndSection();
   }


   if (props.BeginSection("Position"s))
   {
      props.InputFloat2<Light>(
         m_part, "Position"s, //
         [](const Light* light) { return light->m_d.m_vCenter; }, //
         [](Light* light, const Vertex2D& v) { light->Translate(Vertex2D(v.x - light->m_d.m_vCenter.x, v.y - light->m_d.m_vCenter.y)); }, PropertyPane::Unit::VPLength, 1);
      props.InputFloat<Light>(
         m_part, "Height"s, //
         [](const Light* light) { return light->m_d.m_height; }, //
         [](Light* light, float v) { light->m_d.m_height = v; }, PropertyPane::Unit::VPLength, 1);
      props.SurfaceCombo<Light>(
         m_part, "Surface"s, //
         [](const Light* light) { return light->m_d.m_szSurface; }, //
         [](Light* light, const string& v) { light->m_d.m_szSurface = v; }, "Surface this part rests on (defines its base height)");
      props.EndSection();
   }

   if (props.BeginSection("States"s))
   {
      props.Checkbox<Light>(
         m_part, "Blinking Light"s, //
         [](const Light* light) { return light->m_d.m_state == 2; }, //
         [](Light* light, bool v)
         {
            if (v)
               light->m_d.m_state = 2.f;
            else if (light->m_d.m_state == 2.f)
               light->m_d.m_state = 1.f;
            light->setInPlayState(light->m_d.m_state == 2.f ? (float)LightStateBlinking : light->m_d.m_state); // Apply live, like State
         });
      if (const Light* light = props.GetEditedPart<Light>(m_part); light->m_d.m_state == 2.f)
      {
         props.InputString<Light>(
            m_part, "Blink Pattern"s, //
            [](const Light* light) { return light->m_d.m_rgblinkpattern; }, //
            [](Light* light, const string& v) { light->m_d.m_rgblinkpattern = v; }, "On/off steps played in a loop, one character per blink interval ('1' = on, e.g. '10' toggles)");
         props.InputInt<Light>(
            m_part, "Blink Interval (ms)"s, //
            [](const Light* light) { return light->m_d.m_blinkinterval; }, //
            [](Light* light, int v) { light->m_d.m_blinkinterval = v; });
      }
      else
      {
         props.InputFloat<Light>(
            m_part, "State"s, //
            [](const Light* light) { return light->m_d.m_state; }, //
            [](Light* light, float v)
            {
               light->m_d.m_state = v;
               light->m_currentIntensity = light->m_d.m_intensity * light->m_d.m_intensity_scale * light->m_d.m_state;
               light->m_currentFilamentTemperature = bulb_emission_to_filament_temperature(v);
               light->setInPlayState(v > 1.f ? (float)LightStateBlinking : v);
            },
            PropertyPane::Unit::Percent, 1, "Current light state: 0 = off, 1 = on, fractional values dim the light");
      }
      props.InputResURI<Light>(
         m_part, "State Link"s, //
         [](const Light* light) { return light->m_d.m_stateSrcLink; }, //
         [](Light* light, const string& v) { light->m_d.m_stateSrcLink = v; }, ResURIKind::State, "URI linking this light's state to a shared state source (plugin or external controller)");
      props.EndSection();
   }

   if (m_part->GetPTable()->m_liveBaseTable && props.BeginSection("Transient state"s))
   {
      props.InputFloat<Light>(
         m_part, "Intensity Scale"s, //
         [](const Light* light) { return light->m_d.m_intensity_scale; }, //
         [](Light* light, float v) { light->m_d.m_intensity_scale = v; }, PropertyPane::Unit::None, 1, "Runtime multiplier applied to the intensity (transient state, not saved)");
      props.InputFloat<Light>(
         m_part, "Current Intensity"s, //
         [](const Light* light) { return light->m_currentIntensity; }, //
         [](Light* light, float v) { light->m_currentIntensity = v; }, PropertyPane::Unit::None, 1, "Intensity currently emitted by the light (transient state, not saved)");
      props.InputFloat<Light>(
         m_part, "Filament Temperature"s, //
         [](const Light* light) { return static_cast<float>(light->m_currentFilamentTemperature); }, //
         [](Light* light, float v) { light->m_currentFilamentTemperature = static_cast<double>(v); }, PropertyPane::Unit::None, 1,
         "Simulated filament temperature driving the incandescent fader (transient state, not saved)");
      props.EndSection();
   }

   props.TimerSection(m_part);
}

}
