#include <windows.h>
#include <imm.h>
#include <msctf.h>
#include <UIAutomation.h>
#include <oleauto.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <wrl/client.h>

#include <KeyEvent.h>
#include <ResponseParser.h>
#include <WeaselIPC.h>
#include <resource.h>
#include "TrayStatus.h"
#include "PreeditOverlay.h"

namespace {

HHOOK g_keyboard_hook = nullptr;
TrayStatus g_tray;
PreeditOverlay g_overlay;
PreeditMode g_overlay_mode = PreeditMode::Auto;
std::mutex g_preedit_mutex;
std::wstring g_preedit_text;
bool g_toggle_space_down = false;
HANDLE g_single_instance = nullptr;
std::atomic<bool> g_ascii_mode{false};
std::atomic<bool> g_composing{false};
// Construct lazily so the user-mode IPC namespace is set before GetPipeName().
weasel::Client& UserModeClient() {
  static weasel::Client client;
  return client;
}
ITfInputProcessorProfileMgr* g_profile_manager = nullptr;
bool g_user_enabled = true;
bool g_intercepting = false;
HWND g_foreground_window = nullptr;
std::array<bool, 256> g_swallowed_keys{};

struct QueuedKey {
  weasel::KeyEvent event;
  DWORD virtual_key = 0;
  DWORD scan_code = 0;
  DWORD flags = 0;
  bool key_up = false;
  bool language_toggle = false;
  bool toggle_activation = false;
  HWND destination = nullptr;
  uint64_t epoch = 0;
  ULONGLONG event_time_ms = 0;
};

constexpr size_t kMaxQueuedKeys = 128;
// Mark our own SendInput events without excluding other input injectors such
// as Remote Desktop and accessibility tools from the input frontend.
constexpr ULONG_PTR kOurInjectedKeyTag = 0x5745534C;
std::mutex g_queue_mutex;
std::condition_variable g_queue_cv;
std::deque<QueuedKey> g_queue;
std::thread g_worker;
std::thread g_caret_worker;
std::mutex g_caret_mutex;
RECT g_uia_caret = {};
HWND g_uia_window = nullptr;
ULONGLONG g_uia_timestamp = 0;
HWND g_sampled_font_window = nullptr;
int g_sampled_font_height_px = 0;
std::atomic<bool> g_stop_worker{false};
std::atomic<bool> g_worker_ready{false};
std::atomic<bool> g_fail_open{false};
std::atomic<HWND> g_requested_window{nullptr};
std::atomic<HWND> g_worker_window{nullptr};
std::atomic<uint64_t> g_epoch{0};
std::atomic<ULONGLONG> g_oldest_queued_ms{0};
std::atomic<ULONGLONG> g_inflight_ms{0};

constexpr int kToggleHotkeyId = 1;
constexpr int kExitHotkeyId = 2;
constexpr UINT kDefaultToggleModifiers = MOD_CONTROL | MOD_ALT;
constexpr UINT kDefaultToggleVirtualKey = VK_F11;
constexpr wchar_t kUserModeRegistryKey[] = L"Software\\Rime\\Weasel\\UserMode";

struct HotkeyConfig {
  UINT modifiers = kDefaultToggleModifiers;
  UINT virtual_key = kDefaultToggleVirtualKey;
};

std::wstring ModuleDirectory() {
  wchar_t path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
  if (length == 0 || length == ARRAYSIZE(path))
    return L".";
  std::wstring result(path, length);
  const auto separator = result.find_last_of(L"\\/");
  return separator == std::wstring::npos ? L"." : result.substr(0, separator);
}

bool LaunchServer() {
  const std::wstring directory = ModuleDirectory();
  const std::wstring server = directory + L"\\WeaselServer.exe";
  if (GetFileAttributesW(server.c_str()) == INVALID_FILE_ATTRIBUTES)
    return false;

  // ShellExecute may delegate to Explorer, losing the custom environment.
  // CreateProcess inherits WEASEL_USER_MODE=1 into the portable server.
  std::wstring command_line = L"\"" + server + L"\"";
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  if (!CreateProcessW(server.c_str(), command_line.data(), nullptr, nullptr,
                      FALSE, 0, nullptr, directory.c_str(), &startup, &process))
    return false;

  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}

bool LaunchDeployer(const wchar_t* arguments) {
  const std::wstring directory = ModuleDirectory();
  const std::wstring executable = directory + L"\\WeaselDeployer.exe";
  if (GetFileAttributesW(executable.c_str()) == INVALID_FILE_ATTRIBUTES)
    return false;

  std::wstring command_line = L"\"" + executable + L"\"";
  if (arguments && *arguments) {
    command_line += L" ";
    command_line += arguments;
  }

  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  if (!CreateProcessW(executable.c_str(), command_line.data(), nullptr, nullptr,
                      FALSE, 0, nullptr, directory.c_str(), &startup, &process))
    return false;

  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
}

// The overlay only needs the preedit string, not the server-owned
// candidate list. Do not hand a Context to the generic deserializer: doing so
// also deserializes ctx.cand via Boost archive on every keystroke, and was
// crashing the portable frontend on Windows (0xc0000005).
bool DrainResponse(std::wstring* commit = nullptr,
                   bool capture_preedit = false) {
  weasel::Status status{};
  weasel::ResponseParser parser(commit, nullptr, &status);
  std::wstring preedit;
  const auto response = [&](LPWSTR buffer, DWORD length) -> bool {
    const bool parsed = parser(buffer, length);
    if (!parsed || !capture_preedit)
      return parsed;

    // Reuse the already-received IPC buffer. Limit all reads to its length;
    // the final "." line terminates the response, not a NUL byte.
    constexpr wchar_t prefix[] = L"ctx.preedit=";
    constexpr size_t prefix_size = (sizeof(prefix) / sizeof(wchar_t)) - 1;
    for (size_t pos = 0; pos < length;) {
      size_t end = pos;
      while (end < length && buffer[end] != L'\n' && buffer[end] != L'\0')
        ++end;
      const size_t count = end - pos;
      if (count == 1 && buffer[pos] == L'.')
        break;
      if (count >= prefix_size &&
          wcsncmp(buffer + pos, prefix, prefix_size) == 0) {
        preedit = unescape_string(
            std::wstring(buffer + pos + prefix_size, count - prefix_size));
      }
      if (end == length || buffer[end] == L'\0')
        break;
      pos = end + 1;
    }
    return parsed;
  };
  const bool result = UserModeClient().GetResponseData(response);
  if (result) {
    g_ascii_mode = status.ascii_mode;
    g_composing = status.composing;
    if (capture_preedit) {
      std::lock_guard<std::mutex> guard(g_preedit_mutex);
      g_preedit_text = status.composing ? std::move(preedit) : L"";
    }
  }
  return result;
}

DWORD ReadUserModeDword(const wchar_t* name, DWORD fallback) {
  DWORD value = fallback;
  DWORD size = sizeof(value);
  RegGetValueW(HKEY_CURRENT_USER, kUserModeRegistryKey, name, RRF_RT_REG_DWORD,
               nullptr, &value, &size);
  return value;
}

PreeditMode LoadOverlayMode() {
  const DWORD value = ReadUserModeDword(L"OverlayMode", 1);
  return value <= 3 ? static_cast<PreeditMode>(value) : PreeditMode::Auto;
}

void SetOverlayMode(int value) {
  if (value < 0 || value > 3)
    return;
  g_overlay_mode = static_cast<PreeditMode>(value);
  g_overlay.Hide();
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kUserModeRegistryKey, 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
    const DWORD selected = static_cast<DWORD>(value);
    RegSetValueExW(key, L"OverlayMode", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&selected), sizeof(selected));
    RegCloseKey(key);
  }
}

HotkeyConfig LoadToggleHotkey() {
  HotkeyConfig config;
  config.modifiers =
      ReadUserModeDword(L"ToggleModifiers", kDefaultToggleModifiers);
  config.virtual_key =
      ReadUserModeDword(L"ToggleVirtualKey", kDefaultToggleVirtualKey);
  return config;
}

void SaveToggleHotkey(const HotkeyConfig& config) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kUserModeRegistryKey, 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
    return;

  const DWORD modifiers = config.modifiers;
  const DWORD virtual_key = config.virtual_key;
  RegSetValueExW(key, L"ToggleModifiers", 0, REG_DWORD,
                 reinterpret_cast<const BYTE*>(&modifiers), sizeof(modifiers));
  RegSetValueExW(key, L"ToggleVirtualKey", 0, REG_DWORD,
                 reinterpret_cast<const BYTE*>(&virtual_key),
                 sizeof(virtual_key));
  RegCloseKey(key);
}

bool RegisterToggleHotkey() {
  const HotkeyConfig requested = LoadToggleHotkey();

  struct Candidate {
    HotkeyConfig config;
    const wchar_t* name;
  };

  const std::array<Candidate, 4> candidates = {{
      {requested, L"the configured hotkey"},
      {{MOD_CONTROL | MOD_ALT, VK_F11}, L"Ctrl+Alt+F11"},
      {{MOD_CONTROL | MOD_SHIFT, VK_F11}, L"Ctrl+Shift+F11"},
      {{MOD_CONTROL | MOD_ALT, VK_F10}, L"Ctrl+Alt+F10"},
  }};

  for (size_t index = 0; index < candidates.size(); ++index) {
    const auto& candidate = candidates[index];
    bool duplicate = false;
    for (size_t previous = 0; previous < index; ++previous) {
      if (candidates[previous].config.modifiers == candidate.config.modifiers &&
          candidates[previous].config.virtual_key ==
              candidate.config.virtual_key) {
        duplicate = true;
        break;
      }
    }
    if (duplicate)
      continue;

    if (!RegisterHotKey(nullptr, kToggleHotkeyId,
                        candidate.config.modifiers | MOD_NOREPEAT,
                        candidate.config.virtual_key)) {
      continue;
    }

    if (index != 0) {
      SaveToggleHotkey(candidate.config);
      std::wstring message =
          L"The configured Weasel User Mode toggle hotkey is already in use. "
          L"Using ";
      message += candidate.name;
      message += L" instead. This fallback has been saved for this user.";
      MessageBoxW(nullptr, message.c_str(), L"Weasel User Mode",
                  MB_OK | MB_ICONINFORMATION);
    }
    return true;
  }

  MessageBoxW(
      nullptr,
      L"Could not reserve a global toggle hotkey. Weasel User Mode will still "
      L"run enabled. You can exit it with Ctrl+Alt+F12 or Task Manager.",
      L"Weasel User Mode", MB_OK | MB_ICONWARNING);
  return false;
}

void SmokeTrace(const char* stage);
std::atomic<bool> g_smoke_test_mode{false};
void TraceBootstrap(const char* stage) {
  if (g_smoke_test_mode.load())
    SmokeTrace(stage);
}
bool ConnectServer() {
  TraceBootstrap("bootstrap:before-echo");
  if (UserModeClient().Echo())
    return true;
  TraceBootstrap("bootstrap:before-disconnect");
  UserModeClient().Disconnect();
  TraceBootstrap("bootstrap:before-connect");
  if (UserModeClient().Connect()) {
    TraceBootstrap("bootstrap:connected-to-existing");
    UserModeClient().StartSession();
    TraceBootstrap("bootstrap:session-started");
    DrainResponse();
    TraceBootstrap("bootstrap:response-drained");
    if (UserModeClient().Echo())
      return true;
    UserModeClient().Disconnect();
  }

  TraceBootstrap("bootstrap:before-launch-server");
  if (!LaunchServer())
    return false;
  TraceBootstrap("bootstrap:server-launched");
  for (int attempt = 0; attempt < 30; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!UserModeClient().Connect())
      continue;
    TraceBootstrap("bootstrap:connected-after-launch");
    UserModeClient().StartSession();
    TraceBootstrap("bootstrap:session-started-after-launch");
    DrainResponse();
    TraceBootstrap("bootstrap:response-drained-after-launch");
    if (UserModeClient().Echo())
      return true;
    UserModeClient().Disconnect();
  }
  return false;
}

// Optional stage breadcrumbs make GUI-subsystem crashes diagnosable in CI.
void SmokeTrace(const char* stage) {
  wchar_t path[MAX_PATH] = {};
  const DWORD length = GetTempPathW(ARRAYSIZE(path), path);
  if (!length || length >= ARRAYSIZE(path))
    return;
  std::wstring full(path);
  full += L"weasel-user-mode-smoke.log";
  HANDLE file = CreateFileW(full.c_str(), FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return;
  DWORD written = 0;
  WriteFile(file, stage, static_cast<DWORD>(strlen(stage)), &written, nullptr);
  WriteFile(file, "\r\n", 2, &written, nullptr);
  CloseHandle(file);
}

// Verifies the *real* packaged WeaselServer/Rime IPC path without a TSF IME.
// This is not a desktop keyboard-hook/Notepad end-to-end test.
int RunIpcSmokeTest() {
  g_smoke_test_mode = true;
  SmokeTrace("starting");
  if (!ConnectServer())
    return 10;

  SmokeTrace("connected");
  std::wstring committed;
  bool any_key_handled = false;
  bool preedit_observed = false;
  for (const wchar_t ch : std::wstring(L"nihao")) {
    const weasel::KeyEvent key(static_cast<UINT>(ch), 0);
    SmokeTrace("before-typing");
    any_key_handled |= UserModeClient().ProcessKeyEvent(key);
    SmokeTrace("after-process-key");
    std::wstring segment;
    DrainResponse(&segment, true);
    {
      std::lock_guard<std::mutex> guard(g_preedit_mutex);
      preedit_observed |= !g_preedit_text.empty();
    }
    SmokeTrace("after-key-response-with-preedit");
    committed += segment;
  }

  const weasel::KeyEvent space(ibus::space, 0);
  SmokeTrace("before-space");
  any_key_handled |= UserModeClient().ProcessKeyEvent(space);
  std::wstring segment;
  DrainResponse(&segment, true);
  SmokeTrace("after-space");
  committed += segment;

  // Ensure mode switching works without any scheme-specific Ctrl+Space binding.
  SmokeTrace("before-enable-ascii");
  UserModeClient().TrayCommand(ID_WEASELTRAY_ENABLE_ASCII);
  SmokeTrace("after-enable-ascii");
  UserModeClient().ProcessKeyEvent(weasel::KeyEvent(ibus::Control_L, 0));
  DrainResponse();
  SmokeTrace("after-ascii-on-response");
  const bool ascii_on = g_ascii_mode.load();
  SmokeTrace("before-disable-ascii");
  UserModeClient().TrayCommand(ID_WEASELTRAY_DISABLE_ASCII);
  UserModeClient().ProcessKeyEvent(weasel::KeyEvent(ibus::Control_L, 0));
  DrainResponse();
  SmokeTrace("after-ascii-off-response");
  const bool ascii_off = !g_ascii_mode.load();

  SmokeTrace("before-shutdown");
  UserModeClient().EndSession();
  UserModeClient().ShutdownServer();
  UserModeClient().Disconnect();
  SmokeTrace("after-shutdown");

  bool contains_chinese = false;
  for (const wchar_t ch : committed) {
    if (ch >= 0x3400 && ch <= 0x9fff) {
      contains_chinese = true;
      break;
    }
  }

  if (!any_key_handled)
    return 11;
  if (!contains_chinese)
    return 12;
  if (!ascii_on || !ascii_off)
    return 20;
  if (!preedit_observed)
    return 21;
  return 0;
}

void SendUnicode(const std::wstring& text) {
  for (const wchar_t ch : text) {
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wScan = ch;
    inputs[0].ki.dwFlags = KEYEVENTF_UNICODE;
    inputs[0].ki.dwExtraInfo = kOurInjectedKeyTag;
    inputs[1] = inputs[0];
    inputs[1].ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput(ARRAYSIZE(inputs), inputs, sizeof(INPUT));
  }
}

// Retrieve the actual screen-space insertion point exposed by Chromium/Edge,
// Firefox and other UI Automation text providers. The UIA provider is out of
// process and may stall; it must never be called from the keyboard hook or the
// Rime IPC worker. A dedicated MTA sampler publishes a recent rectangle.
bool BoundsForTextRange(IUIAutomationTextRange* range, RECT& result) {
  if (!range)
    return false;
  SAFEARRAY* coords = nullptr;
  if (FAILED(range->GetBoundingRectangles(&coords)) || !coords)
    return false;
  long lower = 0;
  long upper = -1;
  const bool valid = SafeArrayGetDim(coords) == 1 &&
                     SUCCEEDED(SafeArrayGetLBound(coords, 1, &lower)) &&
                     SUCCEEDED(SafeArrayGetUBound(coords, 1, &upper)) &&
                     (upper - lower + 1) >= 4;
  if (!valid) {
    SafeArrayDestroy(coords);
    return false;
  }
  // For a nonempty selection, anchor to the final visible text rectangle.
  long position = upper - 3;
  double rect[4] = {};
  for (int i = 0; i < 4; ++i) {
    long index = position + i;
    if (FAILED(SafeArrayGetElement(coords, &index, &rect[i]))) {
      SafeArrayDestroy(coords);
      return false;
    }
  }
  SafeArrayDestroy(coords);
  if (!std::isfinite(rect[0]) || !std::isfinite(rect[1]) ||
      !std::isfinite(rect[2]) || !std::isfinite(rect[3]) || rect[3] <= 0 ||
      rect[3] > 400 || rect[2] < 0)
    return false;
  result.left = static_cast<LONG>(std::lround(rect[0]));
  result.top = static_cast<LONG>(std::lround(rect[1]));
  result.right = result.left + 2;
  result.bottom = result.top + static_cast<LONG>(std::lround(rect[3]));
  return true;
}

// UI Automation FontSize is expressed in points. Convert to physical pixels
// using the target window's DPI; do not confuse it with the candidate
// theme's independent font size or the system-DPI of the overlay process.
int TextRangeFontHeight(IUIAutomationTextRange* range, HWND foreground) {
  if (!range)
    return 0;
  VARIANT value;
  VariantInit(&value);
  int height_px = 0;
  if (SUCCEEDED(range->GetAttributeValue(UIA_FontSizeAttributeId, &value))) {
    double points = 0;
    if (value.vt == VT_R8)
      points = value.dblVal;
    else if (value.vt == VT_R4)
      points = value.fltVal;
    else if (value.vt == VT_I4)
      points = static_cast<double>(value.lVal);
    if (std::isfinite(points) && points >= 6 && points <= 72) {
      const UINT dpi = GetDpiForWindow(foreground);
      height_px =
          static_cast<int>(std::lround(points * (dpi ? dpi : 96) / 72.0));
    }
  }
  VariantClear(&value);
  return height_px;
}

// Standard Win32 edit controls (including classic Notepad and many native
// widgets) expose an HFONT. Limit the cross-process message timeout so a
// hung editor can never delay the keyboard hook or Rime IPC worker.
int FocusedControlFontHeight(HWND foreground) {
  if (!foreground)
    return 0;
  const DWORD thread_id = GetWindowThreadProcessId(foreground, nullptr);
  GUITHREADINFO info = {};
  info.cbSize = sizeof(info);
  if (!thread_id || !GetGUIThreadInfo(thread_id, &info))
    return 0;
  HWND focus = info.hwndFocus ? info.hwndFocus : foreground;
  DWORD_PTR font_result = 0;
  if (!SendMessageTimeoutW(focus, WM_GETFONT, 0, 0,
                           SMTO_ABORTIFHUNG | SMTO_BLOCK, 35, &font_result) ||
      !font_result)
    return 0;
  LOGFONTW logfont = {};
  if (GetObjectW(reinterpret_cast<HFONT>(font_result), sizeof(logfont),
                 &logfont) != sizeof(logfont))
    return 0;
  return std::abs(static_cast<int>(logfont.lfHeight));
}

// Chrome/Edge occasionally expose an edit box but report an empty or stale
// degenerate TextPattern caret range. Its ValuePattern still contains the
// committed text. For single-line browser edits, estimate the insertion
// point at the end of that value instead of falling back to the top-left
// corner of the widget, which covers already committed text.
bool BrowserValueEndCaret(IUIAutomationElement* focused,
                          HWND foreground,
                          RECT& result,
                          int& font_height_px) {
  if (!focused || !foreground)
    return false;
  wchar_t window_class[80] = {};
  GetClassNameW(foreground, window_class, ARRAYSIZE(window_class));
  if (_wcsicmp(window_class, L"Chrome_WidgetWin_1") != 0)
    return false;

  CONTROLTYPEID control_type = 0;
  if (FAILED(focused->get_CurrentControlType(&control_type)) ||
      control_type != UIA_EditControlTypeId)
    return false;

  Microsoft::WRL::ComPtr<IUIAutomationValuePattern> value_pattern;
  if (FAILED(focused->GetCurrentPatternAs(UIA_ValuePatternId,
                                          IID_PPV_ARGS(&value_pattern))) ||
      !value_pattern)
    return false;
  BSTR value = nullptr;
  if (FAILED(value_pattern->get_CurrentValue(&value)) || !value)
    return false;
  const UINT text_length = SysStringLen(value);
  const bool multiline = wcschr(value, L'\n') || wcschr(value, L'\r');
  RECT bounds = {};
  const bool has_bounds =
      SUCCEEDED(focused->get_CurrentBoundingRectangle(&bounds));
  RECT window = {};
  const bool within_window =
      GetWindowRect(foreground, &window) && bounds.left >= window.left - 8 &&
      bounds.top >= window.top - 8 && bounds.right <= window.right + 8 &&
      bounds.bottom <= window.bottom + 8;
  if (!has_bounds || !within_window || multiline || text_length > 4096 ||
      bounds.right <= bounds.left + 40 || bounds.bottom <= bounds.top + 10) {
    SysFreeString(value);
    return false;
  }

  font_height_px = font_height_px > 0 ? font_height_px : 16;
  const int height = PreeditOverlay::NormalizeFontHeight(font_height_px);
  // Chromium's TextPattern selection rectangle can remain at the start
  // of the input indefinitely. Unlike GDI text-width estimation, the
  // DocumentRange's final *character* has a real on-screen rectangle:
  // this handles proportional fonts, mixed CJK/Latin and horizontal
  // scrolling without accumulating an error on each committed character.
  Microsoft::WRL::ComPtr<IUIAutomationTextPattern> glyph_pattern;
  if (text_length > 0 &&
      SUCCEEDED(focused->GetCurrentPatternAs(UIA_TextPatternId,
                                             IID_PPV_ARGS(&glyph_pattern))) &&
      glyph_pattern) {
    Microsoft::WRL::ComPtr<IUIAutomationTextRange> document;
    if (SUCCEEDED(glyph_pattern->get_DocumentRange(&document)) && document) {
      BSTR document_text = nullptr;
      const bool same_text = SUCCEEDED(document->GetText(-1, &document_text)) &&
                             document_text &&
                             SysStringLen(document_text) == text_length &&
                             wmemcmp(document_text, value, text_length) == 0;
      SysFreeString(document_text);
      if (same_text) {
        Microsoft::WRL::ComPtr<IUIAutomationTextRange> last_character;
        if (SUCCEEDED(document->Clone(&last_character)) && last_character) {
          int moved = 0;
          if (SUCCEEDED(last_character->MoveEndpointByRange(
                  TextPatternRangeEndpoint_Start, document.Get(),
                  TextPatternRangeEndpoint_End)) &&
              SUCCEEDED(last_character->MoveEndpointByUnit(
                  TextPatternRangeEndpoint_Start, TextUnit_Character, -1,
                  &moved)) &&
              moved == -1) {
            SAFEARRAY* rectangles = nullptr;
            if (SUCCEEDED(last_character->GetBoundingRectangles(&rectangles)) &&
                rectangles) {
              long lower = 0;
              long upper = -1;
              if (SafeArrayGetDim(rectangles) == 1 &&
                  SUCCEEDED(SafeArrayGetLBound(rectangles, 1, &lower)) &&
                  SUCCEEDED(SafeArrayGetUBound(rectangles, 1, &upper)) &&
                  upper - lower + 1 >= 4) {
                // GetBoundingRectangles stores x/y/width/height doubles.
                double glyph[4] = {};
                bool readable = true;
                for (int i = 0; i < 4; ++i) {
                  long index = upper - 3 + i;
                  if (FAILED(
                          SafeArrayGetElement(rectangles, &index, &glyph[i]))) {
                    readable = false;
                    break;
                  }
                }
                if (readable && std::isfinite(glyph[0]) &&
                    std::isfinite(glyph[1]) && std::isfinite(glyph[2]) &&
                    std::isfinite(glyph[3]) && glyph[2] > 0 && glyph[3] > 0) {
                  const int x =
                      static_cast<int>(std::lround(glyph[0] + glyph[2]));
                  const int y = static_cast<int>(std::lround(glyph[1]));
                  if (x >= bounds.left && x <= bounds.right - 3 &&
                      y >= bounds.top - 5 && y <= bounds.bottom - 5) {
                    result = {x, y, x + 2,
                              y + static_cast<int>(std::lround(glyph[3]))};
                    SafeArrayDestroy(rectangles);
                    SysFreeString(value);
                    return true;
                  }
                }
              }
              SafeArrayDestroy(rectangles);
            }
          }
        }
      }
    }
  }

  // In current Edge builds the selection text range can be stuck at the
  // start of the edit while ValuePattern changes. Its small rectangle is
  // nevertheless a reliable text baseline (observed x=128/y=52 whereas the
  // control bounds start at x=124/y=48). Use it instead of guessing a search
  // icon's padding, which could shift the overlay by an entire character.
  int text_origin_x = bounds.left + 4;
  int line_top = bounds.top + ((bounds.bottom - bounds.top) - height) / 2;
  Microsoft::WRL::ComPtr<IUIAutomationTextPattern> text_pattern;
  if (SUCCEEDED(focused->GetCurrentPatternAs(UIA_TextPatternId,
                                             IID_PPV_ARGS(&text_pattern))) &&
      text_pattern) {
    Microsoft::WRL::ComPtr<IUIAutomationTextRangeArray> selection;
    if (SUCCEEDED(text_pattern->GetSelection(&selection)) && selection) {
      int count = 0;
      if (SUCCEEDED(selection->get_Length(&count)) && count > 0) {
        Microsoft::WRL::ComPtr<IUIAutomationTextRange> selected;
        RECT selection_bounds = {};
        if (SUCCEEDED(selection->GetElement(count - 1, &selected)) &&
            BoundsForTextRange(selected.Get(), selection_bounds) &&
            selection_bounds.left >= bounds.left &&
            selection_bounds.left <= bounds.left + 20) {
          text_origin_x = selection_bounds.left;
          line_top = selection_bounds.top;
        }
      }
    }
  }
  // Older UIA providers may not expose character bounds. Only as a last
  // resort estimate width, and clamp it to the visible input rectangle.
  HDC dc = GetDC(nullptr);
  SIZE text_size = {};
  if (dc) {
    HFONT font =
        CreateFontW(-height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    DEFAULT_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    if (font) {
      HGDIOBJ old = SelectObject(dc, font);
      GetTextExtentPoint32W(dc, value, static_cast<int>(text_length),
                            &text_size);
      SelectObject(dc, old);
      DeleteObject(font);
    }
    ReleaseDC(nullptr, dc);
  }
  SysFreeString(value);

  const int x = (std::min)(bounds.right - 8, text_origin_x + text_size.cx);
  const int y = line_top;
  result = {x, y, x + 2, y + height};
  return true;
}

bool GetAutomationCaret(IUIAutomation* automation,
                        HWND foreground,
                        RECT& rect,
                        int& font_height_px) {
  if (!automation || !foreground)
    return false;
  Microsoft::WRL::ComPtr<IUIAutomationElement> focused;
  if (FAILED(automation->GetFocusedElement(&focused)) || !focused)
    return false;
  int focused_process = 0;
  DWORD foreground_process = 0;
  GetWindowThreadProcessId(foreground, &foreground_process);
  if (FAILED(focused->get_CurrentProcessId(&focused_process)))
    return false;
  if (static_cast<DWORD>(focused_process) != foreground_process) {
    // Chromium UIA text providers may live in a renderer subprocess. Only
    // accept them if the element is entirely inside the foreground browser.
    wchar_t window_class[80] = {};
    GetClassNameW(foreground, window_class, ARRAYSIZE(window_class));
    RECT bounds = {}, window = {};
    if (_wcsicmp(window_class, L"Chrome_WidgetWin_1") != 0 ||
        FAILED(focused->get_CurrentBoundingRectangle(&bounds)) ||
        !GetWindowRect(foreground, &window) || bounds.left < window.left - 8 ||
        bounds.top < window.top - 8 || bounds.right > window.right + 8 ||
        bounds.bottom > window.bottom + 8)
      return false;
  }

  RECT browser_end = {};
  int browser_font_height = font_height_px;
  const bool browser_estimated = BrowserValueEndCaret(
      focused.Get(), foreground, browser_end, browser_font_height);
  const auto accept_caret = [&](IUIAutomationTextRange* range) -> bool {
    if (!BoundsForTextRange(range, rect))
      return false;
    font_height_px = TextRangeFontHeight(range, foreground);
    // If Chromium gives the left edge of a filled text field instead of its
    // real caret, prefer the committed-value end. Keep correct UIA positions.
    if (browser_estimated && browser_end.left > rect.left + 8) {
      rect = browser_end;
      if (!font_height_px)
        font_height_px = browser_font_height;
    }
    return true;
  };

  Microsoft::WRL::ComPtr<IUIAutomationTextPattern2> text2;
  if (SUCCEEDED(focused->GetCurrentPatternAs(UIA_TextPattern2Id,
                                             IID_PPV_ARGS(&text2))) &&
      text2) {
    BOOL active = FALSE;
    Microsoft::WRL::ComPtr<IUIAutomationTextRange> range;
    if (SUCCEEDED(text2->GetCaretRange(&active, &range)) && active &&
        accept_caret(range.Get())) {
      return true;
    }
  }

  Microsoft::WRL::ComPtr<IUIAutomationTextPattern> text;
  if (SUCCEEDED(focused->GetCurrentPatternAs(UIA_TextPatternId,
                                             IID_PPV_ARGS(&text))) &&
      text) {
    Microsoft::WRL::ComPtr<IUIAutomationTextRangeArray> selection;
    if (SUCCEEDED(text->GetSelection(&selection)) && selection) {
      int count = 0;
      if (SUCCEEDED(selection->get_Length(&count)) && count > 0) {
        Microsoft::WRL::ComPtr<IUIAutomationTextRange> range;
        if (SUCCEEDED(selection->GetElement(count - 1, &range)) &&
            accept_caret(range.Get())) {
          return true;
        }
      }
    }
  }
  if (browser_estimated) {
    rect = browser_end;
    font_height_px = browser_font_height;
    return true;
  }
  return false;
}

void CaretSamplerLoop() {
  const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(initialized))
    return;
  Microsoft::WRL::ComPtr<IUIAutomation> automation;
  CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                   IID_PPV_ARGS(&automation));
  while (!g_stop_worker.load()) {
    const HWND foreground = g_requested_window.load();
    RECT caret = {};
    int font_height_px = 0;
    const bool found =
        foreground && foreground == GetForegroundWindow() &&
        GetAutomationCaret(automation.Get(), foreground, caret, font_height_px);
    if (foreground && foreground == GetForegroundWindow() && !font_height_px)
      font_height_px = FocusedControlFontHeight(foreground);
    {
      std::lock_guard<std::mutex> guard(g_caret_mutex);
      g_uia_window = found ? foreground : nullptr;
      g_uia_caret = caret;
      g_uia_timestamp = found ? GetTickCount64() : 0;
      if (foreground != g_sampled_font_window) {
        g_sampled_font_window = foreground;
        g_sampled_font_height_px = 0;
      }
      if (font_height_px > 0)
        g_sampled_font_height_px =
            PreeditOverlay::NormalizeFontHeight(font_height_px);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
  }
  automation.Reset();
  CoUninitialize();
}

RECT GetInputPosition() {
  RECT result = {};
  const HWND foreground = GetForegroundWindow();
  // UIA positions are physical screen coordinates, not HWND client-relative.
  // Prefer a fresh caret rectangle from the dedicated accessibility sampler.
  {
    std::lock_guard<std::mutex> guard(g_caret_mutex);
    const ULONGLONG now = GetTickCount64();
    if (foreground && g_uia_window == foreground && g_uia_timestamp &&
        now - g_uia_timestamp < 700)
      return g_uia_caret;
  }
  const DWORD thread_id = GetWindowThreadProcessId(foreground, nullptr);
  GUITHREADINFO info = {};
  info.cbSize = sizeof(info);
  if (thread_id != 0 && GetGUIThreadInfo(thread_id, &info) && info.hwndCaret) {
    POINT top_left = {info.rcCaret.left, info.rcCaret.top};
    POINT bottom_right = {info.rcCaret.right, info.rcCaret.bottom};
    if (ClientToScreen(info.hwndCaret, &top_left) &&
        ClientToScreen(info.hwndCaret, &bottom_right)) {
      result = {top_left.x, top_left.y, bottom_right.x, bottom_right.y};
      if (result.bottom <= result.top)
        result.bottom = result.top + 20;
      return result;
    }
  }

  // Never position candidates at the mouse pointer when caret information
  // is unavailable. Prefer the focused input widget's top-left text area.
  const HWND focus = info.hwndFocus ? info.hwndFocus : foreground;
  RECT bounds = {};
  if (focus && GetClientRect(focus, &bounds)) {
    POINT anchor = {bounds.left + 6, bounds.top + 6};
    if (ClientToScreen(focus, &anchor)) {
      result = {anchor.x, anchor.y, anchor.x + 2, anchor.y + 20};
      return result;
    }
  }
  if (foreground && GetWindowRect(foreground, &bounds)) {
    result = {bounds.left + 12, bounds.top + 36, bounds.left + 14,
              bounds.top + 56};
  }
  return result;
}

bool GetOverlayCaret(RECT& result);

int GetTargetFontHeight(const RECT& caret) {
  const HWND foreground = GetForegroundWindow();
  {
    std::lock_guard<std::mutex> guard(g_caret_mutex);
    if (foreground && foreground == g_sampled_font_window &&
        g_sampled_font_height_px > 0)
      return g_sampled_font_height_px;
  }
  // Some apps have no accessible font object. The caret's screen-space
  // height is a closer approximation than a fixed 20pt overlay.
  const int caret_height = caret.bottom - caret.top;
  if (caret_height >= 12 && caret_height <= 96)
    return PreeditOverlay::NormalizeFontHeight(
        static_cast<int>(std::lround(caret_height * 0.82)));
  return PreeditOverlay::NormalizeFontHeight(0);
}

// Candidates and the visual preedit are independent topmost windows. Without
// extra room, Weasel places the popup just 6 px beneath the caret's bottom,
// which often falls INSIDE our 20pt preedit window. Never fix that by raising
// either window every frame: they will flicker as their z-orders alternate.
// Reserve the overlay's text height in the IPC caret anchor instead, keeping
// the two windows spatially separate even before the first preedit response.
RECT GetCandidateInputPosition() {
  RECT result = {};
  if (!GetOverlayCaret(result))
    result = GetInputPosition();
  if (g_overlay_mode == PreeditMode::Off || result.bottom <= result.top)
    return result;
  const int height = GetTargetFontHeight(result);
  const int overlay_top =
      PreeditOverlay::OverlayTop(result, g_overlay_mode, height);
  const int overlay_bottom =
      overlay_top + PreeditOverlay::OverlayHeight(g_overlay_mode, height);
  // WeaselPanel adds 6px internally. Subtract 4px here so its candidate
  // window begins just 2px below the rendered preedit, not 10-20px away.
  result.top = overlay_top;
  result.bottom = overlay_bottom - 4;
  return result;
}

// Overlay must never follow the approximate fallback position used by the
// candidate popup. Only use a caret actually reported by UIA or Win32.
bool GetOverlayCaret(RECT& result) {
  const HWND foreground = GetForegroundWindow();
  if (!foreground)
    return false;
  {
    std::lock_guard<std::mutex> guard(g_caret_mutex);
    if (g_uia_window == foreground && g_uia_timestamp &&
        GetTickCount64() - g_uia_timestamp < 700) {
      result = g_uia_caret;
      return result.bottom > result.top;
    }
  }
  GUITHREADINFO info = {};
  info.cbSize = sizeof(info);
  const DWORD thread_id = GetWindowThreadProcessId(foreground, nullptr);
  if (!thread_id || !GetGUIThreadInfo(thread_id, &info) || !info.hwndCaret)
    return false;
  POINT top_left = {info.rcCaret.left, info.rcCaret.top};
  POINT bottom_right = {info.rcCaret.right, info.rcCaret.bottom};
  if (!ClientToScreen(info.hwndCaret, &top_left) ||
      !ClientToScreen(info.hwndCaret, &bottom_right))
    return false;
  result = {top_left.x, top_left.y, bottom_right.x,
            (std::max)(bottom_right.y, top_left.y + 18)};
  return true;
}

void RefreshOverlay() {
  if (!g_user_enabled || !g_intercepting || g_fail_open.load() ||
      g_overlay_mode == PreeditMode::Off) {
    g_overlay.Hide();
    return;
  }
  RECT caret = {};
  if (!GetOverlayCaret(caret)) {
    // A guessed top-left widget position is not a text caret. On Edge this
    // covered the original text after each commit; never render there.
    g_overlay.Hide();
    return;
  }
  std::wstring preedit;
  {
    std::lock_guard<std::mutex> guard(g_preedit_mutex);
    preedit = g_preedit_text;
  }
  g_overlay.Update(preedit, caret, g_overlay_mode, GetTargetFontHeight(caret));
}

// ImmIsIME() can return TRUE for a regular keyboard HKL (verified on the
// Windows 10 test VM: English US = 0x04090409). The real layout must be
// classified using its IME DLL/profile ID, not ImmIsIME() alone.
bool IsPlainKeyboardLayout(HKL layout) {
  if (!layout)
    return false;
  const WORD device = HIWORD(reinterpret_cast<UINT_PTR>(layout));
  // IMM and TSF IME pseudo-HKLs are commonly represented with an E0xx/F0xx
  // device ID. Regular variants (US, Dvorak, international) are not.
  if ((device & 0xf000) == 0xe000 || (device & 0xf000) == 0xf000)
    return false;
  wchar_t ime_dll[MAX_PATH] = {};
  // A real legacy IME has an associated IME module; plain keyboard layouts
  // have none, even on systems where ImmIsIME() claims otherwise.
  return ImmGetIMEFileNameW(layout, ime_dll, ARRAYSIZE(ime_dll)) == 0;
}

bool ForegroundUsesPlainKeyboardLayout() {
  const HWND foreground = GetForegroundWindow();
  if (!foreground)
    return false;

  DWORD process_id = 0;
  const DWORD thread_id = GetWindowThreadProcessId(foreground, &process_id);
  if (!thread_id)
    return false;
  if (process_id == GetCurrentProcessId())
    return false;

  HKL layout = GetKeyboardLayout(thread_id);
  if (!layout) {
    // Legacy conhost/cmd consoles sometimes report HKL=0 even for the active
    // US keyboard (observed on the Windows 10 VM). Resolve the frontend GUI
    // thread's layout, but ONLY for a verified console window. Other unknown
    // windows must still fail open instead of risking double IME processing.
    wchar_t class_name[64] = {};
    GetClassNameW(foreground, class_name, ARRAYSIZE(class_name));
    if (_wcsicmp(class_name, L"ConsoleWindowClass") != 0)
      return false;
    layout = GetKeyboardLayout(GetCurrentThreadId());
    if (!layout) {
      const HWND shell = GetShellWindow();
      const DWORD shell_thread = GetWindowThreadProcessId(shell, nullptr);
      if (shell_thread)
        layout = GetKeyboardLayout(shell_thread);
    }
  }
  if (!layout)
    return false;

  // On this VM ImmIsIME(0x04090409) incorrectly reports TRUE, disabling
  // interception even with English (US) selected.
  if (!IsPlainKeyboardLayout(layout))
    return false;

  // UIPI rejects SendInput into a higher-integrity process. Do not swallow
  // keys destined for a window we cannot reliably send text back into.
  const auto integrity_level = [](HANDLE process) -> DWORD {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token))
      return 0;
    DWORD required = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &required);
    std::vector<BYTE> data(required);
    DWORD level = 0;
    if (required >= sizeof(TOKEN_MANDATORY_LABEL) &&
        GetTokenInformation(token, TokenIntegrityLevel, data.data(), required,
                            &required)) {
      auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(data.data());
      const UCHAR count = *GetSidSubAuthorityCount(label->Label.Sid);
      if (count)
        level = *GetSidSubAuthority(label->Label.Sid, count - 1);
    }
    CloseHandle(token);
    return level;
  };
  static const DWORD own_level = integrity_level(GetCurrentProcess());
  HANDLE process =
      OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
  if (!process)
    return false;
  const DWORD target_level = integrity_level(process);
  CloseHandle(process);
  if (!own_level || !target_level || target_level > own_level)
    return false;

  GUITHREADINFO info = {};
  info.cbSize = sizeof(info);
  if (GetGUIThreadInfo(thread_id, &info) && info.hwndFocus) {
    wchar_t class_name[64] = {};
    GetClassNameW(info.hwndFocus, class_name, _countof(class_name));
    // A focused Win32 button is not a text editor (for example, Notepad's
    // Save / Don't Save confirmation). Never steal its accelerator keys.
    if (_wcsicmp(class_name, L"Button") == 0)
      return false;
    if ((_wcsicmp(class_name, L"Edit") == 0 ||
         _wcsnicmp(class_name, L"RichEdit", 8) == 0) &&
        (GetWindowLongPtrW(info.hwndFocus, GWL_STYLE) & ES_PASSWORD))
      return false;
  }

  return true;
}

void StopInterception() {
  g_requested_window.store(nullptr);
  ++g_epoch;
  g_queue_cv.notify_one();
  g_intercepting = false;
  g_swallowed_keys.fill(false);
  g_toggle_space_down = false;
  {
    std::lock_guard<std::mutex> guard(g_preedit_mutex);
    g_preedit_text.clear();
  }
  g_overlay.Hide();
}

void UpdateTray() {
  if (!g_user_enabled) {
    g_tray.Update(TrayMode::Off, L"已關閉");
  } else if (g_fail_open.load()) {
    g_tray.Update(TrayMode::Error, L"Rime 反應過慢；請關閉再啟用以重試");
  } else if (!g_worker_ready.load()) {
    g_tray.Update(TrayMode::Paused, L"正在連接 Rime 服務");
  } else if (!g_intercepting) {
    g_tray.Update(TrayMode::Paused, L"原生 IME／受保護欄位／無輸入焦點");
  } else if (g_ascii_mode.load()) {
    g_tray.Update(TrayMode::English, L"Rime 英文模式");
  } else {
    g_tray.Update(TrayMode::Chinese, L"Rime 中文模式");
  }
}

bool RefreshForegroundSession() {
  const HWND foreground = GetForegroundWindow();
  const bool allowed =
      g_user_enabled && foreground && ForegroundUsesPlainKeyboardLayout();
  const HWND requested = allowed ? foreground : nullptr;
  g_foreground_window = requested;
  if (g_requested_window.exchange(requested) != requested) {
    ++g_epoch;
    g_swallowed_keys.fill(false);
    g_queue_cv.notify_one();
  }
  g_intercepting = requested && g_worker_ready.load() &&
                   g_worker_window.load() == requested && !g_fail_open.load();
  UpdateTray();
  return g_intercepting;
}

void ToggleUserMode() {
  g_user_enabled = !g_user_enabled;
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kUserModeRegistryKey, 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
    const DWORD enabled = g_user_enabled ? 1 : 0;
    RegSetValueExW(key, L"StartEnabled", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&enabled), sizeof(enabled));
    RegCloseKey(key);
  }
  if (!g_user_enabled)
    StopInterception();
  else {
    g_fail_open = false;
    RefreshForegroundSession();
  }
  UpdateTray();
  // Enabling is asynchronous: the worker still needs to establish focus.
  // Reporting PAUSED here would often be stale by the time the notice shows.
  // The tray icon continues to report the actual Chinese/English/paused state.
  g_tray.Notice(g_user_enabled
                    ? L"User Mode 已啟用。請查看系統匣的中／英／停圖示。"
                    : L"User Mode 已停用。");
}

void SetKeyStateForEvent(std::array<BYTE, 256>& state, DWORD vk, bool key_up) {
  if (vk >= state.size())
    return;
  if (key_up)
    state[vk] &= ~0x80;
  else
    state[vk] |= 0x80;

  if (vk == VK_LSHIFT || vk == VK_RSHIFT)
    state[VK_SHIFT] = state[vk];
  else if (vk == VK_LCONTROL || vk == VK_RCONTROL)
    state[VK_CONTROL] = state[vk];
  else if (vk == VK_LMENU || vk == VK_RMENU)
    state[VK_MENU] = state[vk];
}

bool ConvertLowLevelKey(const KBDLLHOOKSTRUCT& hook,
                        bool key_up,
                        weasel::KeyEvent& result) {
  DWORD lparam = 1 | ((hook.scanCode & 0xff) << 16);
  if (hook.flags & LLKHF_EXTENDED)
    lparam |= 1 << 24;
  if (key_up)
    lparam |= (1u << 30) | (1u << 31);

  std::array<BYTE, 256> state = {};
  GetKeyboardState(state.data());
  // Keyboard state belongs to the hook's message-pump thread, not to the
  // foreground application. Reconstruct physical modifiers explicitly.
  constexpr std::array<int, 9> modifiers = {
      VK_SHIFT,    VK_LSHIFT, VK_RSHIFT, VK_CONTROL, VK_LCONTROL,
      VK_RCONTROL, VK_MENU,   VK_LMENU,  VK_RMENU};
  for (const int vk : modifiers) {
    if (GetAsyncKeyState(vk) & 0x8000)
      state[vk] |= 0x80;
    else
      state[vk] &= 0x7f;
  }
  SetKeyStateForEvent(state, hook.vkCode, key_up);
  KeyInfo info(static_cast<LPARAM>(lparam));
  const HWND foreground = GetForegroundWindow();
  const DWORD foreground_thread =
      foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
  const HKL foreground_layout =
      foreground_thread ? GetKeyboardLayout(foreground_thread) : nullptr;
  const bool translated = ConvertKeyEvent(hook.vkCode, info, state.data(),
                                          result, foreground_layout);
  // Ctrl+grave is Rime's scheme switcher, including the Shift variant.
  // ToUnicodeEx may return a nonprintable control character (or fail) for
  // this OEM key when Ctrl is physically down. Preserve its Ctrl/Shift mask
  // but explicitly send Rime the X11 grave keysym.
  if ((state[VK_CONTROL] & 0x80) && hook.vkCode == VK_OEM_3) {
    result.keycode = ibus::grave;
    return true;
  }
  return translated;
}

bool IsPressed(int vk) {
  return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

bool ShouldBypassSystemShortcut(const KBDLLHOOKSTRUCT& hook) {
  // The low-level hook runs before the async key state is updated. Explicitly
  // bypass the modifier event itself, not only subsequent shortcut keys.
  switch (hook.vkCode) {
    case VK_LWIN:
    case VK_RWIN:
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
      return true;
  }

  // Windows/Alt shortcuts are reserved. Allow Rime's Ctrl+grave menu,
  // Ctrl+Space language toggle (when bound by a scheme), and Onion's
  // Ctrl+punctuation, Ctrl+number, Ctrl+Enter and Ctrl+arrows, while
  // preserving application shortcuts such as Ctrl+C, Ctrl+V and Ctrl+S.
  if (IsPressed(VK_LWIN) || IsPressed(VK_RWIN))
    return true;
  if (IsPressed(VK_MENU))
    return !(g_composing.load() && hook.vkCode == 'V');
  if (!IsPressed(VK_CONTROL))
    return false;
  if (g_composing.load()) {
    switch (hook.vkCode) {
      // Onion's Emacs-style editing and candidate paging bindings only
      // override application shortcuts while Rime is already composing.
      case 'P':
      case 'N':
      case 'B':
      case 'F':
      case 'A':
      case 'E':
      case 'D':
      case 'K':
      case 'H':
      case 'G':
      case 'V':
        return false;
    }
  }
  switch (hook.vkCode) {
    case VK_SPACE:  // Ctrl+Space is handled by Rime, not the Windows IME
    case VK_OEM_3:
    case VK_OEM_COMMA:
    case VK_OEM_PERIOD:
    case VK_OEM_2:
    case VK_OEM_1:
    case VK_OEM_7:
    case VK_OEM_4:
    case VK_OEM_6:
    case VK_OEM_MINUS:
    case VK_OEM_PLUS:
    case VK_RETURN:
    case VK_BACK:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
      return false;
  }
  if ((hook.vkCode >= '0' && hook.vkCode <= '9') ||
      (hook.vkCode >= VK_NUMPAD0 && hook.vkCode <= VK_NUMPAD9))
    return false;
  return true;
}

void ReplayPhysicalKey(const QueuedKey& key) {
  INPUT input = {};
  input.type = INPUT_KEYBOARD;
  input.ki.dwExtraInfo = kOurInjectedKeyTag;
  if (key.scan_code) {
    input.ki.wScan = static_cast<WORD>(key.scan_code);
    input.ki.dwFlags = KEYEVENTF_SCANCODE;
    if (key.flags & LLKHF_EXTENDED)
      input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
  } else {
    input.ki.wVk = static_cast<WORD>(key.virtual_key);
  }
  if (key.key_up)
    input.ki.dwFlags |= KEYEVENTF_KEYUP;
  if (SendInput(1, &input, sizeof(input)) != 1)
    g_fail_open = true;
}

// All Rime IPC occurs on this worker, never from the low-level hook or the
// window message thread. Responses are delivered in the same order as keys.
void WorkerLoop() {
  HWND focused = nullptr;
  bool connected = false;
  std::array<bool, 256> replayed_down{};
  uint64_t processed_epoch = g_epoch.load();
  try {
    while (!g_stop_worker.load()) {
      if (!connected) {
        g_worker_ready = false;
        try {
          connected = ConnectServer();
        } catch (...) {
          connected = false;
        }
        if (!connected) {
          std::this_thread::sleep_for(std::chrono::milliseconds(250));
          continue;
        }
      }

      const HWND requested = g_requested_window.load();
      if (focused != requested) {
        g_worker_window = nullptr;
        if (focused) {
          UserModeClient().ClearComposition();
          DrainResponse();
          UserModeClient().FocusOut();
        }
        focused = requested;
        replayed_down.fill(false);
        if (focused)
          UserModeClient().FocusIn();
        g_worker_window = focused;
      }
      g_worker_ready = true;

      QueuedKey key;
      bool has_key = false;
      {
        std::unique_lock<std::mutex> guard(g_queue_mutex);
        g_queue_cv.wait_for(guard, std::chrono::milliseconds(125), [&] {
          return g_stop_worker.load() || !g_queue.empty() ||
                 g_requested_window.load() != focused;
        });
        if (!g_queue.empty()) {
          key = g_queue.front();
          g_queue.pop_front();
          has_key = true;
        }
        g_oldest_queued_ms =
            g_queue.empty() ? 0 : g_queue.front().event_time_ms;
      }

      if (g_stop_worker.load())
        break;
      if (processed_epoch != g_epoch.load()) {
        processed_epoch = g_epoch.load();
        replayed_down.fill(false);
      }
      if (has_key) {
        if (key.epoch != g_epoch.load() || key.destination != focused ||
            key.destination != GetForegroundWindow() || g_fail_open.load())
          continue;

        // Ctrl+Space is a frontend shortcut, independent of Rime schemas.
        // Only the worker touches IPC; the low-level keyboard hook never
        // blocks.
        if (key.language_toggle) {
          if (key.toggle_activation) {
            UserModeClient().TrayCommand(g_ascii_mode.load()
                                             ? ID_WEASELTRAY_DISABLE_ASCII
                                             : ID_WEASELTRAY_ENABLE_ASCII);
            UserModeClient().ProcessKeyEvent(
                weasel::KeyEvent(ibus::Control_L, 0));  // refresh status/ctx
            DrainResponse();
          }
          continue;
        }

        // The first candidate must use the current caret, before key IPC.
        UserModeClient().UpdateInputPosition(GetCandidateInputPosition());
        g_inflight_ms = GetTickCount64();
        bool handled = false;
        std::wstring commit;
        try {
          handled = UserModeClient().ProcessKeyEvent(key.event);
          DrainResponse(&commit, true);
        } catch (...) {
          connected = false;
          g_worker_ready = false;
          g_fail_open = true;
        }
        g_inflight_ms = 0;

        // The user may have switched applications while the IPC was pending.
        // Never commit to an unrelated application after a focus change.
        if (!connected || key.epoch != g_epoch.load() ||
            key.destination != GetForegroundWindow() || g_fail_open.load())
          continue;
        if (!key.key_up && !handled) {
          ReplayPhysicalKey(key);
          if (key.virtual_key < replayed_down.size())
            replayed_down[key.virtual_key] = true;
        } else if (key.key_up && key.virtual_key < replayed_down.size() &&
                   replayed_down[key.virtual_key]) {
          ReplayPhysicalKey(key);
          replayed_down[key.virtual_key] = false;
        }
        if (!commit.empty())
          SendUnicode(commit);
      } else if (focused && focused == GetForegroundWindow()) {
        // Candidate positioning is best-effort; it must never run in the hook.
        UserModeClient().UpdateInputPosition(GetCandidateInputPosition());
      }
    }
  } catch (...) {
    // No exception may escape a std::thread entry point.
    g_fail_open = true;
  }
  g_worker_ready = false;
  g_worker_window = nullptr;
  if (connected) {
    try {
      if (focused) {
        UserModeClient().ClearComposition();
        UserModeClient().FocusOut();
      }
      UserModeClient().EndSession();
      UserModeClient().Disconnect();
    } catch (...) {
      // Process teardown must not throw.
    }
  }
}

LRESULT CALLBACK KeyboardHook(int code, WPARAM wparam, LPARAM lparam) {
  if (code < 0)
    return CallNextHookEx(g_keyboard_hook, code, wparam, lparam);

  const auto& hook = *reinterpret_cast<KBDLLHOOKSTRUCT*>(lparam);
  if ((hook.flags & LLKHF_INJECTED) && hook.dwExtraInfo == kOurInjectedKeyTag)
    return CallNextHookEx(g_keyboard_hook, code, wparam, lparam);

  const bool key_down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
  const bool key_up = wparam == WM_KEYUP || wparam == WM_SYSKEYUP;
  if (!key_down && !key_up)
    return CallNextHookEx(g_keyboard_hook, code, wparam, lparam);

  const bool paired_release = key_up && hook.vkCode < g_swallowed_keys.size() &&
                              g_swallowed_keys[hook.vkCode];
  if (paired_release)
    g_swallowed_keys[hook.vkCode] = false;

  if (!paired_release &&
      (ShouldBypassSystemShortcut(hook) || !key_down || !g_intercepting ||
       !g_worker_ready.load() || g_fail_open.load() ||
       GetForegroundWindow() != g_foreground_window))
    return CallNextHookEx(g_keyboard_hook, code, wparam, lparam);

  // This callback must never call Rime IPC, CreateProcess, SendInput, or wait
  // for the server. It only translates and enqueues a bounded key event.
  weasel::KeyEvent key_event;
  if (!ConvertLowLevelKey(hook, key_up, key_event) && !paired_release)
    return CallNextHookEx(g_keyboard_hook, code, wparam, lparam);
  if (paired_release && !key_event.keycode)
    key_event = weasel::KeyEvent(hook.vkCode, ibus::RELEASE_MASK);

  QueuedKey pending;
  pending.event = key_event;
  pending.virtual_key = hook.vkCode;
  pending.scan_code = hook.scanCode;
  pending.flags = hook.flags;
  pending.key_up = key_up;
  // Remember the key-down classification so releasing Ctrl early does not
  // leak the Space key-up into the target application.
  if (hook.vkCode == VK_SPACE) {
    // Toggle exactly once per physical Ctrl+Space press, not once per
    // autorepeat. Still swallow the matching release if Ctrl is let go first.
    pending.toggle_activation =
        key_down && IsPressed(VK_CONTROL) && !g_toggle_space_down;
    if (key_down && IsPressed(VK_CONTROL))
      g_toggle_space_down = true;
    pending.language_toggle = g_toggle_space_down;
    if (key_up)
      g_toggle_space_down = false;
  }
  pending.destination = g_foreground_window;
  pending.epoch = g_epoch.load();
  pending.event_time_ms = GetTickCount64();

  {
    std::lock_guard<std::mutex> guard(g_queue_mutex);
    if (g_queue.size() >= kMaxQueuedKeys) {
      g_fail_open = true;
      ++g_epoch;
      g_swallowed_keys.fill(false);
      return paired_release
                 ? 1
                 : CallNextHookEx(g_keyboard_hook, code, wparam, lparam);
    }
    if (g_queue.empty())
      g_oldest_queued_ms = pending.event_time_ms;
    g_queue.push_back(pending);
  }
  if (key_down && hook.vkCode < g_swallowed_keys.size())
    g_swallowed_keys[hook.vkCode] = true;
  g_queue_cv.notify_one();

  return 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int) {
  if (!SetEnvironmentVariableW(L"WEASEL_USER_MODE", L"1")) {
    MessageBoxW(nullptr, L"Failed to select the portable IPC namespace.",
                L"Weasel User Mode", MB_OK | MB_ICONERROR);
    return 5;
  }
  if (command_line && wcscmp(command_line, L"--ipc-smoke") == 0)
    return RunIpcSmokeTest();
  if (command_line && wcscmp(command_line, L"--layout-smoke") == 0) {
    if (!IsPlainKeyboardLayout(reinterpret_cast<HKL>(0x04090409)))
      return 18;
    if (IsPlainKeyboardLayout(reinterpret_cast<HKL>(0xE0010404)))
      return 19;
    return 0;
  }
  if (command_line && wcscmp(command_line, L"--settings") == 0)
    return LaunchDeployer(L"") ? 0 : 6;
  if (command_line && wcscmp(command_line, L"--deploy") == 0)
    return LaunchDeployer(L"/deploy") ? 0 : 6;
  const bool test_singleton =
      command_line && wcscmp(command_line, L"--singleton-hold") == 0;
  // Local\ is scoped to the interactive logon session. Only the owner of
  // this session can start a frontend/hook, including after startup races.
  g_single_instance =
      CreateMutexW(nullptr, TRUE, L"Local\\WeaselUserModeFrontendSingleton");
  if (!g_single_instance)
    return 7;
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    if (test_singleton) {
      CloseHandle(g_single_instance);
      return 17;
    }
    HWND existing = FindWindowW(TrayStatus::kWindowClass, nullptr);
    if (existing)
      PostMessageW(existing, TrayStatus::kActivateMessage, 0, 0);
    CloseHandle(g_single_instance);
    return 0;
  }
  if (test_singleton) {
    Sleep(3500);
    ReleaseMutex(g_single_instance);
    CloseHandle(g_single_instance);
    return 0;
  }

  g_overlay_mode = LoadOverlayMode();
  if (!g_tray.Create(
          instance, ToggleUserMode, [] { LaunchDeployer(L""); },
          [] { LaunchDeployer(L"/deploy"); }, [] { PostQuitMessage(0); },
          [](int mode) { SetOverlayMode(mode); },
          [] { return static_cast<int>(g_overlay_mode); })) {
    ReleaseMutex(g_single_instance);
    CloseHandle(g_single_instance);
    return 8;
  }
  // No TSF profile or global injection is needed for this click-through popup.
  if (!g_overlay.Create(instance))
    SetOverlayMode(
        0);  // Never suppress server preedit if overlay is unavailable.
  const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool com_initialized = SUCCEEDED(com_result);
  if (com_initialized) {
    CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                     CLSCTX_INPROC_SERVER, IID_ITfInputProcessorProfileMgr,
                     reinterpret_cast<void**>(&g_profile_manager));
  }

  g_user_enabled = ReadUserModeDword(L"StartEnabled", 1) != 0;
  g_foreground_window = GetForegroundWindow();
  RefreshForegroundSession();
  UpdateTray();

  const bool toggle_hotkey_registered = RegisterToggleHotkey();
  RegisterHotKey(nullptr, kExitHotkeyId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT,
                 VK_F12);
  const UINT_PTR input_mode_timer = SetTimer(nullptr, 1, 125, nullptr);
  const UINT_PTR overlay_timer = SetTimer(nullptr, 2, 40, nullptr);

  g_keyboard_hook =
      SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHook, instance, 0);
  if (!g_keyboard_hook) {
    MessageBoxW(nullptr, L"Could not install the keyboard hook.",
                L"Weasel User Mode", MB_OK | MB_ICONERROR);
    if (input_mode_timer)
      KillTimer(nullptr, input_mode_timer);
    if (overlay_timer)
      KillTimer(nullptr, overlay_timer);
    g_overlay.Destroy();
    UnregisterHotKey(nullptr, kExitHotkeyId);
    if (toggle_hotkey_registered)
      UnregisterHotKey(nullptr, kToggleHotkeyId);
    StopInterception();
    if (g_profile_manager)
      g_profile_manager->Release();
    if (com_initialized)
      CoUninitialize();
    g_tray.Destroy();
    ReleaseMutex(g_single_instance);
    CloseHandle(g_single_instance);
    return 3;
  }
  g_caret_worker = std::thread(CaretSamplerLoop);
  g_worker = std::thread(WorkerLoop);

  MSG message = {};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (message.message == WM_HOTKEY) {
      if (message.wParam == kToggleHotkeyId) {
        ToggleUserMode();
      } else if (message.wParam == kExitHotkeyId) {
        PostQuitMessage(0);
      }
      continue;
    }
    if (message.message == WM_TIMER && message.wParam == overlay_timer) {
      RefreshOverlay();
      continue;
    }
    if (message.message == WM_TIMER && message.wParam == input_mode_timer) {
      const ULONGLONG now = GetTickCount64();
      const ULONGLONG queued = g_oldest_queued_ms.load();
      const ULONGLONG inflight = g_inflight_ms.load();
      if ((queued && now - queued > 750) ||
          (inflight && now - inflight > 750)) {
        // Never let a stalled server indefinitely swallow new keystrokes.
        g_fail_open = true;
        ++g_epoch;
        g_swallowed_keys.fill(false);
      }
      RefreshForegroundSession();
      UpdateTray();
      continue;
    }
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  UnhookWindowsHookEx(g_keyboard_hook);
  g_keyboard_hook = nullptr;
  if (input_mode_timer)
    KillTimer(nullptr, input_mode_timer);
  if (overlay_timer)
    KillTimer(nullptr, overlay_timer);
  g_overlay.Destroy();
  UnregisterHotKey(nullptr, kExitHotkeyId);
  if (toggle_hotkey_registered)
    UnregisterHotKey(nullptr, kToggleHotkeyId);
  StopInterception();
  g_stop_worker = true;
  g_queue_cv.notify_all();
  if (g_worker.joinable()) {
    CancelSynchronousIo(g_worker.native_handle());
    if (WaitForSingleObject(g_worker.native_handle(), 2000) == WAIT_TIMEOUT) {
      // A stuck third-party IPC call must never keep this session hooked.
      g_tray.Destroy();
      ExitProcess(0);
    }
    g_worker.join();
  }
  if (g_caret_worker.joinable()) {
    if (WaitForSingleObject(g_caret_worker.native_handle(), 2000) ==
        WAIT_TIMEOUT) {
      g_tray.Destroy();
      ExitProcess(0);
    }
    g_caret_worker.join();
  }
  if (g_profile_manager) {
    g_profile_manager->Release();
    g_profile_manager = nullptr;
  }
  if (com_initialized)
    CoUninitialize();
  g_tray.Destroy();
  ReleaseMutex(g_single_instance);
  CloseHandle(g_single_instance);
  return 0;
}
