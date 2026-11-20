// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Real Wii Remotes on Xbox (UWP), through a Mayflash DolphinBar in mode 4.
//
// The DolphinBar shows each of its four Wii Remote slots to the console as a
// USB HID device with the Wii Remote's own IDs (057E:0306). The desktop
// Windows code finds them with SetupDi/CreateFile, which an Xbox app can't
// use, so this backend uses the UWP HID API (Windows.Devices.HumanInterfaceDevice)
// instead. Everything above the HID reads and writes is Dolphin's normal real
// Wii Remote code.

#pragma once

#ifdef WINRT_XBOX

// C++/WinRT headers trip C4946 (warnings are errors here).
#pragma warning(disable : 4946)

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <winrt/Windows.Devices.HumanInterfaceDevice.h>
#include <winrt/Windows.Foundation.h>

#include "Core/HW/WiimoteReal/WiimoteReal.h"

namespace WiimoteReal
{
class WiimoteWinRT final : public Wiimote
{
public:
  WiimoteWinRT(std::string device_id,
               winrt::Windows::Devices::HumanInterfaceDevice::HidDevice device);
  ~WiimoteWinRT() override;
  std::string GetId() const override { return m_device_id; }

protected:
  bool ConnectInternal() override;
  void DisconnectInternal() override;
  bool IsConnected() const override;
  void IOWakeup() override;
  int IORead(u8* buf) override;
  int IOWrite(const u8* buf, size_t len) override;

private:
  void Attach(winrt::Windows::Devices::HumanInterfaceDevice::HidDevice device);
  void Detach();

  std::string m_device_id;

  mutable std::mutex m_mutex;
  std::condition_variable m_cv;
  winrt::Windows::Devices::HumanInterfaceDevice::HidDevice m_device{nullptr};
  winrt::event_token m_input_token{};
  std::deque<std::vector<u8>> m_reports;
  bool m_wakeup = false;
  bool m_lost = false;
};

class WiimoteScannerWinRT final : public WiimoteScannerBackend
{
public:
  WiimoteScannerWinRT();
  ~WiimoteScannerWinRT() override;
  bool IsReady() const override;
  void FindWiimotes(std::vector<Wiimote*>&, Wiimote*&) override;
  void Update() override {}
  void RequestStopSearching() override;
};

// One or more lines saying what the DolphinBar search saw last (shown in the Wii settings).
std::string GetWinRTWiimoteStatus();
}  // namespace WiimoteReal

#else
#include "Core/HW/WiimoteReal/IODummy.h"
namespace WiimoteReal
{
using WiimoteScannerWinRT = WiimoteScannerDummy;
inline std::string GetWinRTWiimoteStatus() { return {}; }
}
#endif
