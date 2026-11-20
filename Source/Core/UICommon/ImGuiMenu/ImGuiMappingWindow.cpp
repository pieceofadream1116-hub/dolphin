#include "ImGuiMappingWindow.h"
#include <algorithm>
#include <chrono>
#include <format>
#include <functional>
#include <imgui.h>
#include <imgui_internal.h>
#include <string>
#include <string_view>
#include <vector>
#include "Core/HW/GCPad.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "InputCommon/ControllerEmu/ControlGroup/Attachments.h"
#include "InputCommon/ControllerEmu/Setting/NumericSetting.h"
#include "InputCommon/ControllerEmu/ControlGroup/Buttons.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/ControllerInterface/MappingCommon.h"
#include "InputCommon/InputConfig.h"

// These are the includes for all of the Extensions
#include "Core/HW/WiimoteEmu/Extension/Classic.h"
#include "Core/HW/WiimoteEmu/Extension/DrawsomeTablet.h"
#include "Core/HW/WiimoteEmu/Extension/Drums.h"
#include "Core/HW/WiimoteEmu/Extension/Guitar.h"
#include "Core/HW/WiimoteEmu/Extension/Nunchuk.h"
#include "Core/HW/WiimoteEmu/Extension/Shinkansen.h"
#include "Core/HW/WiimoteEmu/Extension/TaTaCon.h"
#include "Core/HW/WiimoteEmu/Extension/Turntable.h"
#include "Core/HW/WiimoteEmu/Extension/UDrawTablet.h"

namespace ImGuiFrontend
{
static std::vector<std::function<void()>> s_pendingBinds;
// which button is being captured, and since when (so each capture gets its own 15 seconds)
static ::ControlReference* s_capture_ref = nullptr;
static std::chrono::steady_clock::time_point s_capture_since;
// after a capture: when it finished (a new one can't start for a moment, so one press can't
// start the next box too) and which box to put the highlight back on
static std::chrono::steady_clock::time_point s_capture_done;
static ::ControlReference* s_refocus_ref = nullptr;

// Called right after each mapping box is drawn: puts the highlight back on the box that was
// just remapped, instead of letting it jump to the first box (A) on the page.
static void RefocusIfNeeded(::ControlReference* ref)
{
  if (ref == nullptr || ref != s_refocus_ref)
    return;
  ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
  ImGui::GetCurrentContext()->NavCursorVisible = true;
  ImGui::SetScrollHereY(0.5f);
  s_refocus_ref = nullptr;
}

// Live list of what the device is sending right now, e.g. "Cross, R1".
static std::string ActiveInputs(const ciface::Core::DeviceQualifier& dq)
{
  auto device = g_controller_interface.FindDevice(dq);
  if (!device)
    return "(not connected)";
  std::string out;
  for (auto* input : device->Inputs())
  {
    if (!input->IsDetectable())
      continue;
    if (input->GetState() > 0.5)
    {
      if (!out.empty())
        out += ", ";
      out += input->GetName();
    }
  }
  return out.empty() ? "nothing pressed" : out;
}

void ImGuiMappingWindow::Draw(int port, UIState* state)
{
  ImGuiIO& io = ImGui::GetIO();
  if (state->capturingRef)
  {
    io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard);
    io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
  }
  else
  {
    io.ConfigFlags |= (ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard);
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  }

  const char* title =
      state->mappingWindowIsWii ? "Wii Remote Mapping" : "GameCube Controller Mapping";
  ImGui::SetNextWindowSize(ImVec2(1200, 800), ImGuiCond_FirstUseEver);
  if (!ImGui::Begin(title, &state->showMappingWindow))
  {
    ImGui::End();
    return;
  }

  InputConfig* config = state->mappingWindowIsWii ? Wiimote::GetConfig() : Pad::GetConfig();
  ControllerEmu::EmulatedController* controller = config->GetController(port);
  if (!state->capturingRef)
    s_capture_ref = nullptr;
  if (state->capturingRef)
  {
    // Capturing a button. Wait a moment first so the press that opened this box is ignored,
    // then listen ONLY to this port's own device for up to 15 seconds: plenty of time to put
    // one controller down and pick up the other.
    using Clock = std::chrono::steady_clock;
    constexpr auto GRACE = std::chrono::milliseconds(700);
    constexpr auto LISTEN = std::chrono::seconds(15);

    if (s_capture_ref != state->capturingRef && Clock::now() - s_capture_done <
                                                     std::chrono::milliseconds(500))
    {
      // the same press that finished the last box: don't start another one
      state->capturingRef = nullptr;
      ImGui::End();
      return;
    }
    if (s_capture_ref != state->capturingRef)
    {
      s_capture_ref = state->capturingRef;
      s_capture_since = Clock::now();
      state->mappingInputDetector.reset();
    }

    const auto& def = controller->GetDefaultDevice();
    const std::string device_name = def.ToString();
    const auto waited = Clock::now() - s_capture_since;

    if (waited < GRACE)
    {
      ImGui::Text("Get ready...");
      ImGui::TextUnformatted(("Listening to: " + device_name).c_str());
      ImGui::End();
      return;
    }

    if (!state->mappingInputDetector)
    {
      state->mappingInputDetector = std::make_unique<ciface::Core::InputDetector>();
      std::vector<std::string> devices{device_name};
      state->mappingInputDetector->Start(g_controller_interface, devices);
    }

    const auto left = std::chrono::duration_cast<std::chrono::seconds>(LISTEN - (waited - GRACE));
    ImGui::Text("Press the button now (%d seconds left)", static_cast<int>(std::max<long long>(0, left.count())));
    ImGui::TextUnformatted(("Listening only to: " + device_name).c_str());

    ImGui::TextUnformatted(("Sending now: " + ActiveInputs(def)).c_str());
    const bool listening_to_menu_pad = device_name == g_controller_interface.GetDefaultDeviceString();
    if (!listening_to_menu_pad)
      ImGui::TextUnformatted("B on the Xbox controller: cancel");
    if (!listening_to_menu_pad && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
    {
      // eat this B press so it doesn't also close the whole Remap screen
      ImGui::SetKeyOwner(ImGuiKey_GamepadFaceRight, ImGui::GetID("##cancel_capture"),
                         ImGuiInputFlags_LockUntilRelease);
      s_refocus_ref = state->capturingRef;
      s_capture_done = Clock::now();
      state->capturingRef = nullptr;
      s_capture_ref = nullptr;
      state->mappingInputDetector.reset();
      ImGui::End();
      return;
    }

    state->mappingInputDetector->Update(LISTEN, std::chrono::milliseconds(300), LISTEN);
    if (state->mappingInputDetector->IsComplete())
    {
      auto results = state->mappingInputDetector->TakeResults();
      ciface::MappingCommon::RemoveSpuriousTriggerCombinations(&results);
      if (!results.empty())
      {
        auto expr = ciface::MappingCommon::BuildExpression(results, def,
                                                           ciface::MappingCommon::Quote::On);
        // set it right now, on exactly this one box (not later, when things may have changed)
        auto* ref = state->capturingRef;
        const auto lock = ControllerEmu::EmulatedController::GetStateLock();
        ref->SetExpression(expr);
        controller->UpdateSingleControlReference(g_controller_interface, ref);
      }
      // done, or nothing pressed in 15 seconds: stop waiting so the menus never stay locked
      s_refocus_ref = state->capturingRef;
      s_capture_done = Clock::now();
      state->capturingRef = nullptr;
      s_capture_ref = nullptr;
      state->mappingInputDetector.reset();
    }
    ImGui::End();
    return;
  }

  if (state->capturingRef)
  {
    ImGui::SetWindowFocus();
    io.WantCaptureKeyboard = true;
    io.WantCaptureMouse = true;
  }
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
      ImGui::IsKeyPressed(ImGuiKey_Escape))
    state->showMappingWindow = false;

  // easy way out at the top too: saves and closes (B or Menu do the same)
  if (ImGui::Button("Done", ImVec2(160, 0)))
  {
    config->SaveConfig();
    state->showMappingWindow = false;
    ImGui::End();
    return;
  }
  ImGui::SameLine();
  ImGui::TextDisabled("B or Menu: save and close");
  // press any button on the controller and see the name it comes through as
  ImGui::TextUnformatted(
      ("This controller is sending: " + ActiveInputs(controller->GetDefaultDevice())).c_str());
  ImGui::Spacing();

  // The real Extension group (its ui name is "Extension"; looking it up as "Attachments" found
  // nothing, so the MotionPlus box and extension list did nothing and the Extension tab was empty).
  ControllerEmu::Attachments* attachments = nullptr;
  ControllerEmu::NumericSetting<bool>* motion_plus = nullptr;
  if (state->mappingWindowIsWii)
  {
    for (auto& gp : controller->groups)
    {
      if (gp->type != ControllerEmu::GroupType::Attachments)
        continue;
      attachments = static_cast<ControllerEmu::Attachments*>(gp.get());
      for (auto& ns : gp->numeric_settings)
      {
        if (std::string_view(ns->GetININame()) == "Attach MotionPlus")
          motion_plus = static_cast<ControllerEmu::NumericSetting<bool>*>(ns.get());
      }
    }
    // show what's really set (e.g. by a profile), not a stale guess
    if (attachments)
      state->wiimote_extension = static_cast<int>(attachments->GetSelectedAttachment());
    if (motion_plus)
      state->wiimote_motionplus = motion_plus->GetValue();
  }

  if (ImGui::BeginTabBar("MappingTabs"))
  {
    if (state->mappingWindowIsWii)
    {
      if (ImGui::BeginTabItem("General and Options"))
      {
        ImGuiStyle& style = ImGui::GetStyle();
        float footer = ImGui::GetFrameHeightWithSpacing() + style.ItemSpacing.y;
        ImGui::BeginChild("GeneralScroll", ImVec2(0, -footer), true,
                          ImGuiWindowFlags_AlwaysUseWindowPadding);
        auto findGroup = [&](const std::string& name) {
          for (auto& gp : controller->groups)
            if (gp->ui_name == name)
              return gp.get();
          return (ControllerEmu::ControlGroup*)nullptr;
        };
        auto renderControls = [&](ControllerEmu::ControlGroup* g) {
          if (!g)
            return;
          if (ImGui::CollapsingHeader(g->ui_name.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
          {
            for (auto& cptr : g->controls)
            {
              auto* ctl = cptr.get();
              std::string expr = ctl->control_ref->GetExpression();
              ImGui::Text("%s", ctl->ui_name.c_str());
              ImGui::SameLine(150);
              if (expr.empty())
                expr = "(none)";
              std::string lbl = expr + std::format("##ctl{0}", reinterpret_cast<intptr_t>(ctl));
              if (ImGui::Button(lbl.c_str(), ImVec2(180, 0)))
              {
                state->capturingRef = ctl->control_ref.get();
                state->mappingInputDetector.reset();
              }
              RefocusIfNeeded(ctl->control_ref.get());
              ImGui::SameLine();
              ImGui::Text("%.2f", ctl->GetState());
            }
            ImGui::Separator();
          }
        };
        renderControls(findGroup("Buttons"));
        ImGui::Separator();
        renderControls(findGroup("D-Pad"));
        ImGui::Separator();
        renderControls(findGroup("Hotkeys"));
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Extension##WiiMap", ImGuiTreeNodeFlags_DefaultOpen))
        {
          if (ImGui::Checkbox("MotionPlus##WiiMap", &state->wiimote_motionplus))
          {
            if (motion_plus)
            {
              motion_plus->SetValue(state->wiimote_motionplus);
              motion_plus->SetExpressionFromValue();
            }
            config->SaveConfig();
          }
          static const char* ext_names[] = {
              "None",         "Nunchuk",          "Classic Controller", "Drum Kit",   "Guitar Hero",
              "DJ Turntable", "uDraw GameTablet", "Drawsome Tablet",    "Taiko Drum", "Shinkansen"};
          if (ImGui::Combo("Attached Extension##AttachedExt", &state->wiimote_extension, ext_names,
                           IM_ARRAYSIZE(ext_names)))
          {
            if (attachments)
              attachments->SetSelectedAttachment(state->wiimote_extension);
            config->SaveConfig();
          }
          ImGui::Separator();
        }
        if (ImGui::CollapsingHeader("Rumble##WiiMap", ImGuiTreeNodeFlags_DefaultOpen))
        {
          if (auto* rumGroup = findGroup("Rumble"))
          {
            for (auto& cptr : rumGroup->controls)
            {
              auto* ctl = cptr.get();
              std::string expr = ctl->control_ref->GetExpression();
              ImGui::Text("%s", ctl->ui_name.c_str());
              ImGui::SameLine(150);
              if (expr.empty())
                expr = "(none)";
              std::string lbl = expr + std::format("##ctl{0}", reinterpret_cast<intptr_t>(ctl));
              if (ImGui::Button(lbl.c_str(), ImVec2(180, 0)))
              {
                state->capturingRef = ctl->control_ref.get();
                state->mappingInputDetector.reset();
              }
              RefocusIfNeeded(ctl->control_ref.get());
              ImGui::SameLine();
              ImGui::Text("%.2f", ctl->GetState());
            }
            ImGui::Separator();
          }
        }
        ImGui::Separator();
        if (ImGui::CollapsingHeader("Options##WiiMap", ImGuiTreeNodeFlags_DefaultOpen))
        {
          ImGui::Text("Speaker Pan");
          ImGui::SameLine(150);
          if (ImGui::SliderFloat("Speaker Pan##SpeakerPan", &state->wiimote_speaker_pan, -1.0f,
                                 1.0f))
            config->SaveConfig();
          if (ImGui::SliderFloat("Battery##Battery", &state->wiimote_battery, 0.0f, 1.0f))
            config->SaveConfig();
          ImGui::Checkbox("Upright Wii Remote##Upright", &state->wiimote_upright);
          ImGui::SameLine();
          ImGui::Checkbox("Sideways Wii Remote##Sideways", &state->wiimote_sideways);
          if (auto* optGroup = findGroup("Options"))
          {
            for (auto& cptr : optGroup->controls)
            {
              auto* ctl = cptr.get();
              std::string expr = ctl->control_ref->GetExpression();
              ImGui::Text("%s", ctl->ui_name.c_str());
              ImGui::SameLine(150);
              if (expr.empty())
                expr = "(none)";
              std::string lbl = expr + std::format("##ctl{0}", reinterpret_cast<intptr_t>(ctl));
              if (ImGui::Button(lbl.c_str(), ImVec2(180, 0)))
              {
                state->capturingRef = ctl->control_ref.get();
                state->mappingInputDetector.reset();
              }
              RefocusIfNeeded(ctl->control_ref.get());
              ImGui::SameLine();
              ImGui::Text("%.2f", ctl->GetState());
            }
            ImGui::Separator();
          }
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
      }

      if (ImGui::BeginTabItem("Motion Simulation"))
      {
        ImGuiStyle& style = ImGui::GetStyle();
        float footer_height = ImGui::GetFrameHeightWithSpacing() + style.ItemSpacing.y;
        ImGui::BeginChild(ImGui::GetID("MappingScrollAreaScroll"), ImVec2(0, -footer_height), true,
                          ImGuiWindowFlags_AlwaysUseWindowPadding);
        for (auto& group_ptr : controller->groups)
        {
          auto* group = group_ptr.get();
          if (group->type == ControllerEmu::GroupType::MixedTriggers ||
              group->type == ControllerEmu::GroupType::Tilt ||
              group->type == ControllerEmu::GroupType::Shake)
          {
            if (ImGui::CollapsingHeader(group->ui_name.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            {
              for (auto& ctl_ptr : group->controls)
              {
                auto* ctl = ctl_ptr.get();
                std::string expr = ctl->control_ref->GetExpression();
                ImGui::Text("%s", ctl->ui_name.c_str());
                ImGui::SameLine(150);
                if (expr.empty())
                  expr = "(none)";
                std::string btn_label =
                    expr + std::format("##ctl{0}", reinterpret_cast<intptr_t>(ctl));
                if (ImGui::Button(btn_label.c_str(), ImVec2(180, 0)))
                {
                  state->capturingRef = ctl->control_ref.get();
                  state->mappingInputDetector.reset();
                }
                RefocusIfNeeded(ctl->control_ref.get());
                ImGui::SameLine();
                ImGui::Text("%.2f", ctl->GetState());
              }
              ImGui::Separator();
            }
          }
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
      }

      if (state->mappingWindowIsWii &&
          state->wiimote_extension == static_cast<int>(WiimoteEmu::ExtensionNumber::NUNCHUK) &&
          ImGui::BeginTabItem("Extension Motion Simulation"))
      {
        auto findGroup = [&](const std::string& name) {
          for (auto& gp : controller->groups)
            if (gp->ui_name == name)
              return gp.get();
          return (ControllerEmu::ControlGroup*)nullptr;
        };
        auto renderControls = [&](ControllerEmu::ControlGroup* g) {
          if (!g)
            return;
          if (ImGui::CollapsingHeader(g->ui_name.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
          {
            for (auto& cptr : g->controls)
            {
              auto* ctl = cptr.get();
              std::string expr = ctl->control_ref->GetExpression();
              ImGui::Text("%s", ctl->ui_name.c_str());
              ImGui::SameLine(150);
              if (expr.empty())
                expr = "(none)";
              std::string lbl = expr + std::format("##ctl{0}", reinterpret_cast<intptr_t>(ctl));
              if (ImGui::Button(lbl.c_str(), ImVec2(180, 0)))
              {
                state->capturingRef = ctl->control_ref.get();
                state->mappingInputDetector.reset();
              }
              RefocusIfNeeded(ctl->control_ref.get());
              ImGui::SameLine();
              ImGui::Text("%.2f", ctl->GetState());
            }
            ImGui::Separator();
          }
        };
        // Scrollable child
        ImGuiStyle& styleExt = ImGui::GetStyle();
        float footerExt = ImGui::GetFrameHeightWithSpacing() + styleExt.ItemSpacing.y;
        ImGui::BeginChild("ExtMotionScroll", ImVec2(0, -footerExt), true,
                          ImGuiWindowFlags_AlwaysUseWindowPadding);
        renderControls(findGroup("Shake"));
        renderControls(findGroup("Tilt"));
        renderControls(findGroup("Swing"));
        ImGui::EndChild();
        ImGui::EndTabItem();
      }

#ifndef WINRT_XBOX  // Motion Input tab is only available on Platforms with motion controls
      if (ImGui::BeginTabItem("Motion Input"))
      {
        ImGuiStyle& style = ImGui::GetStyle();
        float footer_height = ImGui::GetFrameHeightWithSpacing() + style.ItemSpacing.y;
        ImGui::BeginChild(ImGui::GetID("MappingScrollAreaScroll"), ImVec2(0, -footer_height), true,
                          ImGuiWindowFlags_AlwaysUseWindowPadding);
        for (auto& group_ptr : controller->groups)
        {
          auto* group = group_ptr.get();
          if (group->type == ControllerEmu::GroupType::IMUAccelerometer ||
              group->type == ControllerEmu::GroupType::IMUGyroscope)
          {
            if (ImGui::CollapsingHeader(group->ui_name.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            {
              for (auto& ctl_ptr : group->controls)
              {
                auto* ctl = ctl_ptr.get();
                std::string expr = ctl->control_ref->GetExpression();
                ImGui::Text("%s", ctl->ui_name.c_str());
                ImGui::SameLine(150);
                if (expr.empty())
                  expr = "(none)";
                std::string btn_label =
                    expr + std::format("##ctl{0}", reinterpret_cast<intptr_t>(ctl));
                if (ImGui::Button(btn_label.c_str(), ImVec2(180, 0)))
                {
                  state->capturingRef = ctl->control_ref.get();
                  state->mappingInputDetector.reset();
                }
                RefocusIfNeeded(ctl->control_ref.get());
                ImGui::SameLine();
                ImGui::Text("%.2f", ctl->GetState());
              }
              ImGui::Separator();
            }
          }
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
      }

#endif
      if (ImGui::BeginTabItem("Extension"))
      {
        ImGuiStyle& style = ImGui::GetStyle();
        float footer_height = ImGui::GetFrameHeightWithSpacing() + style.ItemSpacing.y;
        ImGui::BeginChild(ImGui::GetID("MappingScrollAreaScroll"), ImVec2(0, -footer_height), true,
                          ImGuiWindowFlags_AlwaysUseWindowPadding);
        auto renderGroupUI = [&](ControllerEmu::ControlGroup* group) {
          if (!group)
            return;
          if (ImGui::CollapsingHeader(group->ui_name.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
          {
            for (auto& ctl_ptr : group->controls)
            {
              auto* ctl = ctl_ptr.get();
              std::string expr = ctl->control_ref->GetExpression();
              if (expr.empty())
                expr = "(none)";
              ImGui::Text("%s", ctl->ui_name.c_str());
              ImGui::SameLine(150);
              std::string btn = expr + std::format("##ctl{}", reinterpret_cast<intptr_t>(ctl));
              if (ImGui::Button(btn.c_str(), ImVec2(180, 0)))
              {
                state->capturingRef = ctl->control_ref.get();
                state->mappingInputDetector.reset();
              }
              RefocusIfNeeded(ctl->control_ref.get());
              ImGui::SameLine();
              ImGui::Text("%.2f", ctl->GetState());
            }
            ImGui::Separator();
          }
        };
        switch (state->wiimote_extension)
        {
        case 1:  // Nunchuk
          renderGroupUI(Wiimote::GetNunchukGroup(port, WiimoteEmu::NunchukGroup::Buttons));
          renderGroupUI(Wiimote::GetNunchukGroup(port, WiimoteEmu::NunchukGroup::Stick));
          renderGroupUI(Wiimote::GetNunchukGroup(port, WiimoteEmu::NunchukGroup::Tilt));
          renderGroupUI(Wiimote::GetNunchukGroup(port, WiimoteEmu::NunchukGroup::Shake));
          renderGroupUI(Wiimote::GetNunchukGroup(port, WiimoteEmu::NunchukGroup::IMUAccelerometer));
          break;
        case 2:  // Classic
          renderGroupUI(Wiimote::GetClassicGroup(port, WiimoteEmu::ClassicGroup::Buttons));
          renderGroupUI(Wiimote::GetClassicGroup(port, WiimoteEmu::ClassicGroup::DPad));
          renderGroupUI(Wiimote::GetClassicGroup(port, WiimoteEmu::ClassicGroup::LeftStick));
          renderGroupUI(Wiimote::GetClassicGroup(port, WiimoteEmu::ClassicGroup::RightStick));
          renderGroupUI(Wiimote::GetClassicGroup(port, WiimoteEmu::ClassicGroup::Triggers));
          break;
        case 3:  // Drum Kit
          renderGroupUI(Wiimote::GetDrumsGroup(port, WiimoteEmu::DrumsGroup::Stick));
          renderGroupUI(Wiimote::GetDrumsGroup(port, WiimoteEmu::DrumsGroup::Pads));
          renderGroupUI(Wiimote::GetDrumsGroup(port, WiimoteEmu::DrumsGroup::Buttons));
          break;
        case 4:  // Guitar Hero
          renderGroupUI(Wiimote::GetGuitarGroup(port, WiimoteEmu::GuitarGroup::Stick));
          renderGroupUI(Wiimote::GetGuitarGroup(port, WiimoteEmu::GuitarGroup::Strum));
          renderGroupUI(Wiimote::GetGuitarGroup(port, WiimoteEmu::GuitarGroup::Frets));
          renderGroupUI(Wiimote::GetGuitarGroup(port, WiimoteEmu::GuitarGroup::Buttons));
          renderGroupUI(Wiimote::GetGuitarGroup(port, WiimoteEmu::GuitarGroup::Whammy));
          renderGroupUI(Wiimote::GetGuitarGroup(port, WiimoteEmu::GuitarGroup::SliderBar));
          break;
        case 5:  // DJ Turntable
          renderGroupUI(Wiimote::GetTurntableGroup(port, WiimoteEmu::TurntableGroup::Stick));
          renderGroupUI(Wiimote::GetTurntableGroup(port, WiimoteEmu::TurntableGroup::Buttons));
          renderGroupUI(Wiimote::GetTurntableGroup(port, WiimoteEmu::TurntableGroup::EffectDial));
          renderGroupUI(Wiimote::GetTurntableGroup(port, WiimoteEmu::TurntableGroup::LeftTable));
          renderGroupUI(Wiimote::GetTurntableGroup(port, WiimoteEmu::TurntableGroup::RightTable));
          renderGroupUI(Wiimote::GetTurntableGroup(port, WiimoteEmu::TurntableGroup::Crossfade));
          break;
        case 6:  // uDraw GameTablet
          renderGroupUI(Wiimote::GetUDrawTabletGroup(port, WiimoteEmu::UDrawTabletGroup::Buttons));
          renderGroupUI(Wiimote::GetUDrawTabletGroup(port, WiimoteEmu::UDrawTabletGroup::Stylus));
          renderGroupUI(Wiimote::GetUDrawTabletGroup(port, WiimoteEmu::UDrawTabletGroup::Touch));
          break;
        case 7:  // Drawsome Tablet
          renderGroupUI(
              Wiimote::GetDrawsomeTabletGroup(port, WiimoteEmu::DrawsomeTabletGroup::Stylus));
          renderGroupUI(
              Wiimote::GetDrawsomeTabletGroup(port, WiimoteEmu::DrawsomeTabletGroup::Touch));
          break;
        case 8:  // Taiko Drum
          renderGroupUI(Wiimote::GetTaTaConGroup(port, WiimoteEmu::TaTaConGroup::Center));
          renderGroupUI(Wiimote::GetTaTaConGroup(port, WiimoteEmu::TaTaConGroup::Rim));
          break;
        case 9:  // Shinkansen
          renderGroupUI(Wiimote::GetShinkansenGroup(port, WiimoteEmu::ShinkansenGroup::Levers));
          renderGroupUI(Wiimote::GetShinkansenGroup(port, WiimoteEmu::ShinkansenGroup::Buttons));
          renderGroupUI(Wiimote::GetShinkansenGroup(port, WiimoteEmu::ShinkansenGroup::Light));
          break;
        }
        ImGui::EndChild();
        ImGui::EndTabItem();
      }
    }
    else
    {
      if (ImGui::BeginTabItem("GameCube Controller"))
      {
        ImGuiStyle& style = ImGui::GetStyle();
        float footer_height = ImGui::GetFrameHeightWithSpacing() + style.ItemSpacing.y;
        ImGui::BeginChild(ImGui::GetID("MappingScrollAreaScroll"), ImVec2(0, -footer_height), true,
                          ImGuiWindowFlags_AlwaysUseWindowPadding);
        auto findGroup = [&](const std::string& name) {
          for (auto& gp : controller->groups)
            if (gp->ui_name == name)
              return gp.get();
          return (ControllerEmu::ControlGroup*)nullptr;
        };
        auto renderGroupUI = [&](ControllerEmu::ControlGroup* group) {
          if (!group)
            return;
          if (ImGui::CollapsingHeader(group->ui_name.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
          {
            for (auto& ctl_ptr : group->controls)
            {
              auto* ctl = ctl_ptr.get();
              std::string expr = ctl->control_ref->GetExpression();
              if (expr.empty())
                expr = "(none)";
              ImGui::Text("%s", ctl->ui_name.c_str());
              ImGui::SameLine(150);
              std::string btn = expr + std::format("##ctl{}", reinterpret_cast<intptr_t>(ctl));
              if (ImGui::Button(btn.c_str(), ImVec2(180, 0)))
              {
                state->capturingRef = ctl->control_ref.get();
                state->mappingInputDetector.reset();
              }
              RefocusIfNeeded(ctl->control_ref.get());
              ImGui::SameLine();
              ImGui::Text("%.2f", ctl->GetState());
            }
            ImGui::Separator();
          }
        };
        renderGroupUI(findGroup("Buttons"));
        renderGroupUI(findGroup("Main Stick"));
        renderGroupUI(findGroup("C-Stick"));
        renderGroupUI(findGroup("Triggers"));
        renderGroupUI(findGroup("D-Pad"));
        ImGui::EndChild();
        ImGui::EndTabItem();
      }
    }
    ImGui::EndTabBar();
  }

  if (ImGui::Button("Save", ImVec2(80, 0)))
  {
    config->SaveConfig();
    state->showMappingWindow = false;
  }
  ImGui::SameLine();
  if (ImGui::Button("Close", ImVec2(80, 0)))
  {
    state->showMappingWindow = false;
  }

  ImGui::End();
  for (auto& fn : s_pendingBinds)
    fn();
  s_pendingBinds.clear();
}

}  // namespace ImGuiFrontend
