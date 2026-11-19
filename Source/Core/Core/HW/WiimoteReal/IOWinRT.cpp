// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HW/WiimoteReal/IOWinRT.h"

#ifdef WINRT_XBOX

// C++/WinRT's delegate wrappers for lambdas trip C4265 (warnings are errors here).
#pragma warning(disable : 4265 4946)

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <unordered_map>

#include <objbase.h>

#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Gaming.Input.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.Core.h>

#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"

#include <fmt/format.h>
#include "Core/HW/WiimoteCommon/WiimoteConstants.h"
#include "Core/HW/WiimoteCommon/WiimoteHid.h"

namespace WiimoteReal
{
namespace
{
namespace HID = winrt::Windows::Devices::HumanInterfaceDevice;
using winrt::Windows::Devices::Enumeration::DeviceInformation;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Storage::FileAccessMode;
using winrt::Windows::Storage::Streams::Buffer;

using namespace WiimoteCommon;

constexpr u16 NINTENDO_VID = 0x057e;
constexpr u16 WIIMOTE_PID = 0x0306;       // original Wii Remote (and every DolphinBar slot)
constexpr u16 WIIMOTE_PLUS_PID = 0x0330;  // Wii Remote Plus over Bluetooth

constexpr auto OPEN_TIMEOUT = std::chrono::seconds(5);
constexpr auto WRITE_TIMEOUT = std::chrono::seconds(1);
constexpr auto READ_TIMEOUT = std::chrono::milliseconds(200);
constexpr auto PROBE_TIMEOUT = std::chrono::milliseconds(600);

// An empty DolphinBar slot is still a HID device. Don't reopen it on every scan.
constexpr auto EMPTY_SLOT_RETRY = std::chrono::seconds(3);

// What the scanner saw last time, shown under Wiimote Port 1 so we can tell why a remote
// isn't connecting (no DolphinBar seen, the Xbox refused access, or no remote in a slot).
std::mutex s_status_mutex;
std::string s_status = "Dolphin Bar: not searched yet. Pick \"Real Wii Remote (DolphinBar)\".";
std::string s_open_error;

void SetStatus(std::string text)
{
  std::lock_guard lk(s_status_mutex);
  s_status = std::move(text);
}

// The scanner and Wii Remote threads are plain std::threads; WinRT calls need an apartment.
void EnsureApartment()
{
  thread_local bool s_initialized = false;
  if (s_initialized)
    return;
  // S_FALSE / RPC_E_CHANGED_MODE are fine: the thread already has an apartment.
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  s_initialized = true;
}

// AQS for every present HID interface with the Wii Remote's VID/PID, whatever its usage.
std::wstring WiimoteSelector(u16 pid)
{
  return L"System.Devices.InterfaceClassGuid:=\"{4D1E55B2-F16F-11CF-88CB-001111000030}\""
         L" AND System.Devices.InterfaceEnabled:=System.StructuredQueryType.Boolean#True"
         L" AND System.DeviceInterface.Hid.VendorId:=" +
         std::to_wstring(NINTENDO_VID) + L" AND System.DeviceInterface.Hid.ProductId:=" +
         std::to_wstring(pid);
}

// Opening a HID device can need the UI thread the first time (for the access check), so ask the
// app's dispatcher to start the open, then wait for it here. Fall back to opening directly.
HID::HidDevice OpenDevice(const winrt::hstring& id)
{
  EnsureApartment();

  winrt::Windows::Foundation::IAsyncOperation<HID::HidDevice> op{nullptr};

  try
  {
    auto dispatcher =
        winrt::Windows::ApplicationModel::Core::CoreApplication::MainView().Dispatcher();
    if (dispatcher && !dispatcher.HasThreadAccess())
    {
      // Shared, because the UI thread may run this after we've stopped waiting.
      struct Pending
      {
        std::mutex mutex;
        winrt::Windows::Foundation::IAsyncOperation<HID::HidDevice> op{nullptr};
      };
      const auto pending = std::make_shared<Pending>();

      auto queued = dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal,
                                        [pending, id] {
                                          try
                                          {
                                            auto started = HID::HidDevice::FromIdAsync(
                                                id, FileAccessMode::ReadWrite);
                                            std::lock_guard lk(pending->mutex);
                                            pending->op = std::move(started);
                                          }
                                          catch (...)
                                          {
                                          }
                                        });
      if (queued.wait_for(OPEN_TIMEOUT) == AsyncStatus::Completed)
      {
        std::lock_guard lk(pending->mutex);
        op = pending->op;
      }
    }
  }
  catch (...)
  {
    op = nullptr;
  }

  try
  {
    if (!op)
      op = HID::HidDevice::FromIdAsync(id, FileAccessMode::ReadWrite);

    if (op.wait_for(OPEN_TIMEOUT) != AsyncStatus::Completed)
    {
      op.Cancel();
      s_open_error = "timed out";
      return nullptr;
    }
    HID::HidDevice device = op.GetResults();
    if (!device)
      s_open_error = "Xbox refused access";
    return device;
  }
  catch (const winrt::hresult_error& e)
  {
    s_open_error = fmt::format("error {:08X}", static_cast<u32>(e.code()));
    WARN_LOG_FMT(WIIMOTE, "WinRT HID: couldn't open {}: {:08x} {}", WStringToUTF8(id.c_str()),
                 static_cast<u32>(e.code()), WStringToUTF8(e.message().c_str()));
  }
  catch (...)
  {
    s_open_error = "unknown error";
  }
  return nullptr;
}

// Send one Wii Remote output report (buf[0] = report ID, then the payload).
// Returns the number of bytes sent, or 0 on failure.
u32 SendReport(const HID::HidDevice& device, const u8* buf, size_t len)
{
  if (!device || len == 0)
    return 0;

  try
  {
    HID::HidOutputReport report = device.CreateOutputReport(buf[0]);
    const u32 size = report.Data().Length();
    if (size == 0)
      return 0;

    // The HID report is the size the descriptor gives this report ID; pad with zeros.
    Buffer data(size);
    data.Length(size);
    std::memset(data.data(), 0, size);
    std::memcpy(data.data(), buf, std::min<size_t>(len, size));
    report.Data(data);

    auto op = device.SendOutputReportAsync(report);
    if (op.wait_for(WRITE_TIMEOUT) != AsyncStatus::Completed)
    {
      op.Cancel();
      return 0;
    }
    return op.GetResults();
  }
  catch (const winrt::hresult_error& e)
  {
    // The DolphinBar refuses writes to a slot with no Wii Remote in it.
    DEBUG_LOG_FMT(WIIMOTE, "WinRT HID: write failed: {:08x}", static_cast<u32>(e.code()));
  }
  catch (...)
  {
  }
  return 0;
}

// Is a Wii Remote really there? Ask for a status report and wait for it.
bool ProbeWiimote(const HID::HidDevice& device)
{
  // Shared, because the report handler can still be running after we stop listening.
  struct ProbeState
  {
    std::mutex mutex;
    std::condition_variable cv;
    bool got_status = false;
  };
  const auto state = std::make_shared<ProbeState>();

  winrt::event_token token{};
  try
  {
    token = device.InputReportReceived(
        [state](const HID::HidDevice&, const HID::HidInputReportReceivedEventArgs& args) {
          const auto data = args.Report().Data();
          if (data.Length() >= 1 && data.data()[0] == u8(InputReportID::Status))
          {
            std::lock_guard lk(state->mutex);
            state->got_status = true;
            state->cv.notify_all();
          }
        });
  }
  catch (...)
  {
    return false;
  }

  static const u8 request_status[] = {u8(OutputReportID::RequestStatus), 0};
  bool ok = false;
  if (SendReport(device, request_status, sizeof(request_status)) != 0)
  {
    std::unique_lock lk(state->mutex);
    ok = state->cv.wait_for(lk, PROBE_TIMEOUT, [&] { return state->got_status; });
  }

  try
  {
    device.InputReportReceived(token);
  }
  catch (...)
  {
  }
  return ok;
}

std::unordered_map<std::string, std::chrono::steady_clock::time_point> s_empty_slots;
}  // namespace

WiimoteScannerWinRT::WiimoteScannerWinRT()
{
  EnsureApartment();
}

WiimoteScannerWinRT::~WiimoteScannerWinRT() = default;

bool WiimoteScannerWinRT::IsReady() const
{
  return true;
}

void WiimoteScannerWinRT::FindWiimotes(std::vector<Wiimote*>& found_wiimotes, Wiimote*&)
{
  EnsureApartment();

  const auto now = std::chrono::steady_clock::now();
  static int s_scans = 0;
  ++s_scans;

  int slots = 0, opened = 0, answered = 0, waiting = 0, connected = 0;
  std::string usages, problems;

  // Every Nintendo HID interface the Xbox shows us (whatever its product or usage).
  auto props = winrt::single_threaded_vector<winrt::hstring>(std::vector<winrt::hstring>{
      L"System.DeviceInterface.Hid.ProductId", L"System.DeviceInterface.Hid.UsagePage",
      L"System.DeviceInterface.Hid.UsageId"});
  winrt::Windows::Foundation::Collections::IVectorView<DeviceInformation> devices{nullptr};
  try
  {
    const std::wstring selector =
        L"System.Devices.InterfaceClassGuid:=\"{4D1E55B2-F16F-11CF-88CB-001111000030}\""
        L" AND System.Devices.InterfaceEnabled:=System.StructuredQueryType.Boolean#True"
        L" AND System.DeviceInterface.Hid.VendorId:=" +
        std::to_wstring(NINTENDO_VID);
    auto op = DeviceInformation::FindAllAsync(winrt::hstring(selector), props);
    if (op.wait_for(OPEN_TIMEOUT) != AsyncStatus::Completed)
    {
      op.Cancel();
      SetStatus(fmt::format("Dolphin Bar: search {} timed out.", s_scans));
      return;
    }
    devices = op.GetResults();
  }
  catch (const winrt::hresult_error& e)
  {
    SetStatus(fmt::format("Dolphin Bar: device search failed (error {:08X}).",
                          static_cast<u32>(e.code())));
    return;
  }

  for (const DeviceInformation& info : devices)
  {
    auto prop = [&info](const wchar_t* name) -> u16 {
      try
      {
        auto v = info.Properties().TryLookup(name);
        return v ? winrt::unbox_value_or<u16>(v, 0) : 0;
      }
      catch (...)
      {
        return 0;
      }
    };
    const u16 pid = prop(L"System.DeviceInterface.Hid.ProductId");
    const u16 page = prop(L"System.DeviceInterface.Hid.UsagePage");
    const u16 usage = prop(L"System.DeviceInterface.Hid.UsageId");
    if (pid != WIIMOTE_PID && pid != WIIMOTE_PLUS_PID)
      continue;

    ++slots;
    const std::string u = fmt::format("{:04X}:{:04X}", page, usage);
    if (usages.find(u) == std::string::npos)
      usages += (usages.empty() ? "" : ", ") + u;

    const winrt::hstring hid = info.Id();
    const std::string id = WStringToUTF8(hid.c_str());

    if (!IsNewWiimote(id))
    {
      ++connected;
      continue;
    }

    const auto empty = s_empty_slots.find(id);
    if (empty != s_empty_slots.end() && now < empty->second)
    {
      ++waiting;
      continue;
    }

    s_open_error.clear();
    HID::HidDevice device = OpenDevice(hid);
    if (!device)
    {
      if (problems.find(s_open_error) == std::string::npos)
        problems += (problems.empty() ? "" : ", ") + s_open_error;
      s_empty_slots[id] = now + EMPTY_SLOT_RETRY;
      continue;
    }
    ++opened;

    if (!ProbeWiimote(device))
    {
      // An empty DolphinBar slot (or a remote that isn't awake yet).
      device.Close();
      s_empty_slots[id] = now + EMPTY_SLOT_RETRY;
      continue;
    }
    ++answered;

    s_empty_slots.erase(id);
    NOTICE_LOG_FMT(WIIMOTE, "WinRT HID: found Wii Remote {:04x}:{:04x} at {}", NINTENDO_VID, pid,
                   id);
    found_wiimotes.push_back(new WiimoteWinRT(id, std::move(device)));
  }

  std::string text;
  if (slots == 0)
  {
    text = fmt::format("Dolphin Bar: not seen (search {}). Is it plugged in and on mode 4?", s_scans);
    // Does the Xbox show it as a game controller instead?
    try
    {
      for (const auto& pad : winrt::Windows::Gaming::Input::RawGameController::RawGameControllers())
      {
        if (pad.HardwareVendorId() == NINTENDO_VID)
          text += fmt::format("\nXbox lists it as a game controller ({:04X}:{:04X}).",
                              pad.HardwareVendorId(), pad.HardwareProductId());
      }
    }
    catch (...)
    {
    }
  }
  else
  {
    text = fmt::format("Dolphin Bar: {} slot(s) seen [{}], search {}.", slots, usages, s_scans);
    if (connected)
      text += fmt::format("\nWii Remote connected: {}.", connected);
    if (answered)
      text += fmt::format("\nFound {} Wii Remote(s) just now.", answered);
    if (!problems.empty())
      text += "\nCan't open a slot: " + problems + ".";
    else if (opened && !answered && !connected)
      text += fmt::format("\nOpened {} slot(s), no Wii Remote answered. Press 1+2 on the remote.",
                          opened);
    else if (waiting && !connected)
      text += "\nWaiting to try the slots again...";
  }
  SetStatus(std::move(text));
}

std::string GetWinRTWiimoteStatus()
{
  std::lock_guard lk(s_status_mutex);
  return s_status;
}

WiimoteWinRT::WiimoteWinRT(std::string device_id, HID::HidDevice device)
    : m_device_id(std::move(device_id))
{
  Attach(std::move(device));
}

WiimoteWinRT::~WiimoteWinRT()
{
  Shutdown();
  Detach();
}

void WiimoteWinRT::Attach(HID::HidDevice device)
{
  std::lock_guard lk(m_mutex);
  m_device = std::move(device);
  m_lost = false;
  m_reports.clear();
  if (!m_device)
    return;

  try
  {
    m_input_token = m_device.InputReportReceived(
        [this](const HID::HidDevice&, const HID::HidInputReportReceivedEventArgs& args) {
          const auto data = args.Report().Data();
          const u32 size = std::min<u32>(data.Length(), MAX_PAYLOAD - 1);
          if (size == 0)
            return;

          std::vector<u8> report(data.data(), data.data() + size);
          std::lock_guard report_lk(m_mutex);
          // Don't let reports pile up if nobody is reading.
          if (m_reports.size() > 64)
            m_reports.pop_front();
          m_reports.push_back(std::move(report));
          m_cv.notify_all();
        });
  }
  catch (...)
  {
    m_device = nullptr;
  }
}

void WiimoteWinRT::Detach()
{
  HID::HidDevice device{nullptr};
  winrt::event_token token{};
  {
    std::lock_guard lk(m_mutex);
    device = std::move(m_device);
    m_device = nullptr;
    token = m_input_token;
    m_input_token = {};
    m_reports.clear();
    m_wakeup = true;
    m_cv.notify_all();
  }

  if (!device)
    return;
  try
  {
    device.InputReportReceived(token);
    device.Close();
  }
  catch (...)
  {
  }
}

bool WiimoteWinRT::ConnectInternal()
{
  {
    std::lock_guard lk(m_mutex);
    if (m_device && !m_lost)
      return true;
  }

  Detach();

  HID::HidDevice device = OpenDevice(winrt::to_hstring(m_device_id));
  if (!device)
  {
    ERROR_LOG_FMT(WIIMOTE, "WinRT HID: could not reconnect to Wii Remote at {}", m_device_id);
    return false;
  }
  Attach(std::move(device));
  return IsConnected();
}

void WiimoteWinRT::DisconnectInternal()
{
  Detach();
}

bool WiimoteWinRT::IsConnected() const
{
  std::lock_guard lk(m_mutex);
  return m_device != nullptr && !m_lost;
}

void WiimoteWinRT::IOWakeup()
{
  std::lock_guard lk(m_mutex);
  m_wakeup = true;
  m_cv.notify_all();
}

// buf receives [0xA1, report ID, data...]. Returns its length, -1 for "nothing yet", 0 on error.
int WiimoteWinRT::IORead(u8* buf)
{
  std::unique_lock lk(m_mutex);
  if (!m_device || m_lost)
    return 0;

  m_cv.wait_for(lk, READ_TIMEOUT, [this] { return !m_reports.empty() || m_wakeup || !m_device; });
  m_wakeup = false;

  if (!m_device || m_lost)
    return 0;
  if (m_reports.empty())
    return -1;

  const std::vector<u8> report = std::move(m_reports.front());
  m_reports.pop_front();

  buf[0] = WR_SET_REPORT | BT_INPUT;
  std::memcpy(buf + 1, report.data(), report.size());
  return static_cast<int>(report.size()) + 1;
}

// buf is [0xA2, report ID, data...]. Returns bytes written, or 0 on error.
int WiimoteWinRT::IOWrite(const u8* buf, size_t len)
{
  HID::HidDevice device{nullptr};
  {
    std::lock_guard lk(m_mutex);
    if (!m_device || m_lost || len < 2)
      return 0;
    device = m_device;
  }

  const u32 written = SendReport(device, buf + 1, len - 1);
  if (written == 0)
  {
    // The remote was switched off or left the DolphinBar.
    std::lock_guard lk(m_mutex);
    m_lost = true;
    m_cv.notify_all();
    return 0;
  }
  return static_cast<int>(len);
}
}  // namespace WiimoteReal

#endif  // WINRT_XBOX
