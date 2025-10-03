// license:GPLv3+

#include "core/stdafx.h"
#include "implot/implot.h"

#include "NudgeSettingsPage.h"

namespace VPX::InGameUI
{

NudgeSettingsPage::NudgeSettingsPage()
   : InGameUIPage("settings/nudge"s, "Nudge Settings"s, ""s, SaveMode::Global)
{
   InputManager& input = GetInput();

   ////////////////////////////////////////////////////////////////////////////////////////////////

   auto hardwareNudge = std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Header, "Hardware sensor based emulated nudge");
   AddItem(hardwareNudge);

   for (int i = 0; i < 2; i++)
   {
      auto label = std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Info, "Hardware sensor #" + std::to_string(i + 1));
      AddItem(label);

      const auto& nudgeXSensor = input.GetNudgeXSensor(i);
      auto nudgeXItem = std::make_unique<InGameUIItem>(nudgeXSensor->GetLabel(), "Select to define which analog input to use for side nudge."s, nudgeXSensor.get(), 0x7);
      AddItem(nudgeXItem);

      const auto& nudgeYSensor = input.GetNudgeYSensor(i);
      auto nudgeYItem = std::make_unique<InGameUIItem>(nudgeYSensor->GetLabel(), "Select to define which analog input to use for front nudge."s, nudgeYSensor.get(), 0x7);
      AddItem(nudgeYItem);

      auto sensorOrientation = std::make_unique<InGameUIItem>(
         "Sensor "s + std::to_string(i + 1) + " - Orientation"s, "Define sensor orientation"s, 0.f, 360.f, 1.0f, 0.f, "%4.1f deg"s,
         [this, i]() { return RADTOANG(GetInput().GetNudgeOrientation(i)); }, [this, i](float prev, float v) { GetInput().SetNudgeOrientation(i, ANGTORAD(v)); },
         [i](Settings& settings) { settings.DeleteValue(Settings::Player, "NudgeOrientation" + std::to_string(i + 1)); },
         [i](float v, Settings& settings, bool isTableOverride) { settings.SaveValue(Settings::Player, "NudgeOrientation" + std::to_string(i + 1), v, isTableOverride); });
      AddItem(sensorOrientation);

      auto accFilter = std::make_unique<InGameUIItem>(
         "Sensor "s + std::to_string(i + 1) + " - Use Filter"s, "Enable/Disable filtering acquired value to prevent noise"s, false, [this, i]() { return GetInput().IsNudgeFiltered(i); },
         [this, i](bool v) { GetInput().SetNudgeFiltered(i, v); }, InGameUIItem::ResetSetting(Settings::Player, "NudgeFilter"s + std::to_string(i + 1)),
         InGameUIItem::SaveSettingBool(Settings::Player, "NudgeFilter"s + std::to_string(i + 1)));
      AddItem(accFilter);
   }

   ////////////////////////////////////////////////////////////////////////////////////////////////

   auto plumb = std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Header, "Emulated tilt plumb");
   AddItem(plumb);

   auto enablePlumb = std::make_unique<InGameUIItem>(
      "Plumb simulation"s, "Enable/Disable mechanical Tilt plumb simulation"s, false, [this]() { return m_player->m_physics->IsPlumbSimulated(); }, [this](bool v)
      { m_player->m_physics->EnablePlumbSimulation(v); }, InGameUIItem::ResetSetting(Settings::Player, "TiltSensCB"s), InGameUIItem::SaveSettingBool(Settings::Player, "TiltSensCB"s));
   AddItem(enablePlumb);

   auto plumbInertia = std::make_unique<InGameUIItem>(
      "Plumb Inertia", ""s, 0.f, 10.f, 0.01f, 5.f, "%4.2f"s, [this]() { return m_player->m_physics->GetPlumbInertia(); },
      [this](float prev, float v) { m_player->m_physics->SetPlumbInertia(v); }, [](Settings& settings) { settings.DeleteValue(Settings::Player, "TiltInertia"s); },
      [](float v, Settings& settings, bool isTableOverride) { settings.SaveValue(Settings::Player, "TiltInertia"s, v / 0.05f, isTableOverride); });
   AddItem(plumbInertia);

   auto plumbThreshold = std::make_unique<InGameUIItem>(
      "Plumb Threshold", "Define threshold at which a Tilt is caused"s, 0.1f, 1.f, 0.01f, 0.f, "%4.2f"s, [this]() { return m_player->m_physics->GetPlumbTiltThreshold(); },
      [this](float prev, float v) { m_player->m_physics->SetPlumbTiltThreshold(v); }, [](Settings& settings) { settings.DeleteValue(Settings::Player, "TiltSensitivity"s); },
      [](float v, Settings& settings, bool isTableOverride) { settings.SaveValue(Settings::Player, "TiltSensitivity"s, v, isTableOverride); });
   AddItem(plumbThreshold);

   ////////////////////////////////////////////////////////////////////////////////////////////////

   auto keyboardNudge = std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Header, "Keyboard emulated nudge");
   AddItem(keyboardNudge);

   auto legacyNudge = std::make_unique<InGameUIItem>(
      "Legacy Keyboard nudge"s, "Enable/Disable legacy keyboard nudge mode"s, false, [this]() { return m_player->m_physics->IsLegacyKeyboardNudge(); },
      [this](bool v) { m_player->m_physics->SetLegacyKeyboardNudge(v); }, InGameUIItem::ResetSetting(Settings::Player, "EnableLegacyNudge"s),
      InGameUIItem::SaveSettingBool(Settings::Player, "EnableLegacyNudge"s));
   AddItem(legacyNudge);

   auto legacyNudgeStrength = std::make_unique<InGameUIItem>(
      "Legacy Nudge Strength"s, "Strength of nudge when using the legacy keyboard nudge mode"s, 0.f, 90.f, 0.1f, 1.f, "%4.1f"s,
      [this]() { return m_player->m_physics->GetLegacyKeyboardNudgeStrength(); }, [this](float prev, float v) { m_player->m_physics->SetLegacyKeyboardNudgeStrength(v); },
      [](Settings& settings) { settings.DeleteValue(Settings::Player, "LegacyNudgeStrength"); },
      [](float v, Settings& settings, bool isTableOverride) { settings.SaveValue(Settings::Player, "LegacyNudgeStrength", v, isTableOverride); });
   AddItem(legacyNudgeStrength);

   m_nudgeXPlot.m_rolling = true;
   m_nudgeXPlot.m_timeSpan = 5.f;
   m_nudgeXRawPlot.m_rolling = true;
   m_nudgeXRawPlot.m_timeSpan = 5.f;

   m_nudgeYPlot.m_rolling = true;
   m_nudgeYPlot.m_timeSpan = 5.f;
   m_nudgeYRawPlot.m_rolling = true;
   m_nudgeYRawPlot.m_timeSpan = 5.f;
}

void NudgeSettingsPage::Open()
{
   InGameUIPage::Open();
   m_player->m_pininput.AddAxisListener([this]() { AppendPlot(); });
}

void NudgeSettingsPage::Close()
{
   InGameUIPage::Close();
   m_player->m_pininput.ClearAxisListeners();
}

void NudgeSettingsPage::AppendPlot()
{
   const float t = static_cast<float>(msec()) / 1000.f;
   Vertex2D nudge = m_player->m_pininput.GetNudge();
   m_nudgeXPlot.AddPoint(t, nudge.x);
   m_nudgeYPlot.AddPoint(t, nudge.y);
   if (m_player->m_pininput.GetNudgeXSensor(0)->IsMapped())
      m_nudgeXRawPlot.AddPoint(t, m_player->m_pininput.GetNudgeXSensor(0)->GetMapping().GetRawValue());
   if (m_player->m_pininput.GetNudgeYSensor(0)->IsMapped())
      m_nudgeYRawPlot.AddPoint(t, m_player->m_pininput.GetNudgeYSensor(0)->GetMapping().GetRawValue());
}

void NudgeSettingsPage::Render()
{
   InGameUIPage::Render();

   const ImGuiIO& io = ImGui::GetIO();
   const ImGuiStyle& style = ImGui::GetStyle();

   const ImVec2 winSize = ImVec2(GetWindowSize().x, 400.f);
   const float plotHeight = winSize.y / 2 - style.ItemSpacing.x;
   const float plumbSize = m_player->m_physics->IsPlumbSimulated() ? plotHeight : 0.f;
   const float plotWidth = winSize.x - style.WindowPadding.x * 2.f - style.ItemSpacing.x - plumbSize;
   constexpr ImGuiWindowFlags window_flags
      = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
   ImGui::SetNextWindowPos(ImVec2(GetWindowPos().x, GetWindowPos().y - winSize.y - style.ItemSpacing.y));
   ImGui::SetNextWindowBgAlpha(0.5f);
   ImGui::SetNextWindowSize(winSize);
   ImGui::Begin("NudgeOverlay", nullptr, window_flags);

   AppendPlot();

   if (m_nudgeXPlot.HasData() && ImPlot::BeginPlot("##NudgeX", ImVec2(plotWidth, plotHeight), ImPlotFlags_None))
   {
      ImPlot::SetupAxis(ImAxis_X1, nullptr, ImPlotAxisFlags_NoTickLabels);
      ImPlot::SetupAxis(ImAxis_Y1, nullptr, ImPlotAxisFlags_LockMin);
      ImPlot::SetupAxisLimits(ImAxis_Y1, -1.2f, 1.2f, ImGuiCond_Always);
      ImPlot::SetupAxisLimits(ImAxis_X1, 0, m_nudgeXPlot.m_timeSpan, ImGuiCond_Always);
      if (m_nudgeXRawPlot.HasData())
         ImPlot::PlotLine("Sensor", &m_nudgeXRawPlot.m_data[0].x, &m_nudgeXRawPlot.m_data[0].y, m_nudgeXRawPlot.m_data.size(), 0, m_nudgeXRawPlot.m_offset, 2 * sizeof(float));
      ImPlot::PushStyleColor(ImPlotCol_Fill, ImVec4(1, 0, 0, 0.25f));
      ImPlot::PlotLine("Nudge X", &m_nudgeXPlot.m_data[0].x, &m_nudgeXPlot.m_data[0].y, m_nudgeXPlot.m_data.size(), 0, m_nudgeXPlot.m_offset, 2 * sizeof(float));
      ImPlot::PopStyleColor();
      ImPlot::EndPlot();
   }

   ImGui::SameLine();

   if (m_player->m_physics->IsPlumbSimulated())
   {
      const ImVec2 halfSize(plumbSize * 0.5f, plumbSize * 0.5f);
      ImGui::BeginChild("PlumbPos", ImVec2(plumbSize, plumbSize));
      const ImVec2& pos = ImGui::GetWindowPos();
      ImGui::GetWindowDrawList()->AddLine(pos + ImVec2(0.f, halfSize.y), pos + ImVec2(plumbSize, halfSize.y), IM_COL32(128, 128, 128, 255));
      ImGui::GetWindowDrawList()->AddLine(pos + ImVec2(halfSize.x, 0.f), pos + ImVec2(halfSize.y, plumbSize), IM_COL32(128, 128, 128, 255));
      // Tilt circle
      const ImVec2 scale = halfSize * 1.5f;
      const ImVec2 radius = scale * sin(m_player->m_physics->GetPlumbTiltThreshold() * (float)(M_PI * 0.25));
      ImGui::GetWindowDrawList()->AddEllipse(pos + halfSize, radius, IM_COL32(255, 0, 0, 255));
      // Plumb position
      const Vertex3Ds& plumb = m_player->m_physics->GetPlumbPos();
      const ImVec2 plumbPos = pos + halfSize + scale * ImVec2(plumb.x, plumb.y) / m_player->m_physics->GetPlumbPoleLength() + ImVec2(0.5f, 0.5f);
      ImGui::GetWindowDrawList()->AddLine(pos + halfSize, plumbPos, IM_COL32(255, 128, 0, 255));
      ImGui::GetWindowDrawList()->AddCircleFilled(plumbPos, 5.f * m_player->m_liveUI->GetDPI(), IM_COL32(255, 0, 0, 255));
      ImGui::EndChild();
   }

   if (m_nudgeYPlot.HasData() && ImPlot::BeginPlot("##NudgeY", ImVec2(plotWidth, plotHeight), ImPlotFlags_None))
   {
      ImPlot::SetupAxis(ImAxis_X1, nullptr, ImPlotAxisFlags_NoTickLabels);
      ImPlot::SetupAxis(ImAxis_Y1, nullptr, ImPlotAxisFlags_LockMin);
      ImPlot::SetupAxisLimits(ImAxis_Y1, -1.2f, 1.2f, ImGuiCond_Always);
      ImPlot::SetupAxisLimits(ImAxis_X1, 0, m_nudgeYPlot.m_timeSpan, ImGuiCond_Always);
      if (m_nudgeYRawPlot.HasData())
         ImPlot::PlotLine("Sensor", &m_nudgeYRawPlot.m_data[0].x, &m_nudgeYRawPlot.m_data[0].y, m_nudgeYRawPlot.m_data.size(), 0, m_nudgeYRawPlot.m_offset, 2 * sizeof(float));
      ImPlot::PushStyleColor(ImPlotCol_Fill, ImVec4(1, 0, 0, 0.25f));
      ImPlot::PlotLine("Nudge Y", &m_nudgeYPlot.m_data[0].x, &m_nudgeYPlot.m_data[0].y, m_nudgeYPlot.m_data.size(), 0, m_nudgeYPlot.m_offset, 2 * sizeof(float));
      ImPlot::PopStyleColor();
      ImPlot::EndPlot();
   }

   ImGui::SameLine();

   if (m_player->m_physics->IsPlumbSimulated())
   {
      const float margin = 0.2f;
      const float baseSize = plumbSize * (1.f - margin);
      const float radius = baseSize * 0.75f;
      const ImVec2 halfSize(baseSize * 0.5f, baseSize * 0.5f);
      ImGui::BeginChild("PlumbAngle", ImVec2(plumbSize, plumbSize));
      const ImVec2& pos = ImGui::GetWindowPos() + ImVec2(plumbSize * margin * 0.5f, plumbSize * margin * 0.5f);
      const Vertex3Ds& plumb = m_player->m_physics->GetPlumbPos();
      // Tilt limits
      float angle = m_player->m_physics->GetPlumbTiltThreshold() * (float)(M_PI * 0.25);
      ImVec2 plumbPos = pos + ImVec2(halfSize.x + sinf(angle) * radius, cosf(angle) * radius);
      ImGui::GetWindowDrawList()->AddLine(pos + ImVec2(halfSize.x, 0.f), plumbPos, IM_COL32(255, 0, 0, 255));
      plumbPos = pos + ImVec2(halfSize.x - sin(angle) * radius, cos(angle) * radius);
      ImGui::GetWindowDrawList()->AddLine(pos + ImVec2(halfSize.x, 0.f), plumbPos, IM_COL32(255, 0, 0, 255));
      // Plumb position
      angle = atan2f(sqrt(plumb.x * plumb.x + plumb.y * plumb.y), -plumb.z);
      if (const float theta = atan2(plumb.x, plumb.y); (theta + (float)(M_PI / 2.) < 0.f) || (theta + (float)(M_PI / 2.) >= (float)M_PI))
         angle = -angle;
      plumbPos = pos + ImVec2(halfSize.x + sinf(angle) * radius, cosf(angle) * radius);
      ImGui::GetWindowDrawList()->AddLine(pos + ImVec2(halfSize.x, 0.f), plumbPos, IM_COL32(255, 128, 0, 255));
      ImGui::GetWindowDrawList()->AddCircleFilled(plumbPos, 5.f * m_player->m_liveUI->GetDPI(), IM_COL32(255, 0, 0, 255));
      ImGui::EndChild();
   }

   ImGui::End();
}

}
