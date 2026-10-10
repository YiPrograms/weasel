#include <windows.h>
#include <imm.h>
#include <msctf.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <KeyEvent.h>
#include <ResponseParser.h>
#include <WeaselIPC.h>
#include "TrayStatus.h"

namespace {

HHOOK g_keyboard_hook = nullptr;
TrayStatus g_tray;
HANDLE g_single_instance = nullptr;
std::atomic<bool> g_ascii_mode{false};
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

bool DrainResponse(std::wstring* commit = nullptr) {
  weasel::Status status;
  weasel::ResponseParser parser(commit, nullptr, &status);
  const bool result = UserModeClient().GetResponseData(std::ref(parser));
  if (result)
    g_ascii_mode = status.ascii_mode;
  return result;
}

DWORD ReadUserModeDword(const wchar_t* name, DWORD fallback) {
  DWORD value = fallback;
  DWORD size = sizeof(value);
  RegGetValueW(HKEY_CURRENT_USER, kUserModeRegistryKey, name, RRF_RT_REG_DWORD,
               nullptr, &value, &size);
  return value;
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

bool ConnectServer() {
  if (UserModeClient().Echo())
    return true;

  UserModeClient().Disconnect();
  if (UserModeClient().Connect()) {
    UserModeClient().StartSession();
    DrainResponse();
    if (UserModeClient().Echo())
      return true;
    UserModeClient().Disconnect();
  }

  if (!LaunchServer())
    return false;

  for (int attempt = 0; attempt < 30; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!UserModeClient().Connect())
      continue;
    UserModeClient().StartSession();
    DrainResponse();
    if (UserModeClient().Echo())
      return true;
    UserModeClient().Disconnect();
  }
  return false;
}

// Verifies the *real* packaged WeaselServer/Rime IPC path without a TSF IME.
// This is not a desktop keyboard-hook/Notepad end-to-end test.
int RunIpcSmokeTest() {
  if (!ConnectServer())
    return 10;

  std::wstring committed;
  bool any_key_handled = false;
  for (const wchar_t ch : std::wstring(L"nihao")) {
    const weasel::KeyEvent key(static_cast<UINT>(ch), 0);
    any_key_handled |= UserModeClient().ProcessKeyEvent(key);
    std::wstring segment;
    DrainResponse(&segment);
    committed += segment;
  }

  const weasel::KeyEvent space(ibus::space, 0);
  any_key_handled |= UserModeClient().ProcessKeyEvent(space);
  std::wstring segment;
  DrainResponse(&segment);
  committed += segment;

  UserModeClient().EndSession();
  UserModeClient().ShutdownServer();
  UserModeClient().Disconnect();

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

RECT GetInputPosition() {
  RECT result = {};
  const HWND foreground = GetForegroundWindow();
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

  POINT cursor = {};
  GetCursorPos(&cursor);
  result = {cursor.x, cursor.y, cursor.x + 2, cursor.y + 20};
  return result;
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

  const HKL layout = GetKeyboardLayout(thread_id);
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
}

enum class UserModeState {
  kOff,
  kArmed,
  kActive,
};

UserModeState CurrentUserModeState() {
  if (!g_user_enabled)
    return UserModeState::kOff;
  return g_intercepting ? UserModeState::kActive : UserModeState::kArmed;
}

void ShowUserModeState() {
  if (g_fail_open.load()) {
    g_tray.Notice(L"Rime 回應逾時，已停止攔截。請先停用再重新啟用。");
    return;
  }
  const UserModeState state = CurrentUserModeState();
  const wchar_t* message = nullptr;
  switch (state) {
    case UserModeState::kOff:
      message = L"Weasel User Mode: OFF";
      break;
    case UserModeState::kActive:
      message =
          L"Weasel User Mode: ACTIVE\n\nKeyboard input is being handled by "
          L"Rime.";
      break;
    case UserModeState::kArmed:
      message =
          L"Weasel User Mode: PAUSED\n\n目前焦點不是可接管的普通鍵盤輸入 "
          L"（其他 IME、密碼欄位、較高權限程式或 Rime 尚未就緒）。";
      break;
  }
  g_tray.Notice(message);
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
  return ConvertKeyEvent(hook.vkCode, info, state.data(), result,
                         foreground_layout);
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

  // Never steal Windows/app shortcuts. This intentionally means Rime's
  // Ctrl/Alt shortcuts are unavailable in user-mode for now; coexistence with
  // the host desktop takes priority.
  return IsPressed(VK_LWIN) || IsPressed(VK_RWIN) || IsPressed(VK_CONTROL) ||
         IsPressed(VK_MENU);
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

        g_inflight_ms = GetTickCount64();
        bool handled = false;
        std::wstring commit;
        try {
          handled = UserModeClient().ProcessKeyEvent(key.event);
          DrainResponse(&commit);
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
        UserModeClient().UpdateInputPosition(GetInputPosition());
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

  if (!g_tray.Create(
          instance, ToggleUserMode, [] { LaunchDeployer(L""); },
          [] { LaunchDeployer(L"/deploy"); }, [] { PostQuitMessage(0); })) {
    ReleaseMutex(g_single_instance);
    CloseHandle(g_single_instance);
    return 8;
  }
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

  g_keyboard_hook =
      SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHook, instance, 0);
  if (!g_keyboard_hook) {
    MessageBoxW(nullptr, L"Could not install the keyboard hook.",
                L"Weasel User Mode", MB_OK | MB_ICONERROR);
    if (input_mode_timer)
      KillTimer(nullptr, input_mode_timer);
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
  g_worker = std::thread(WorkerLoop);

  MSG message = {};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (message.message == WM_HOTKEY) {
      if (message.wParam == kToggleHotkeyId) {
        ToggleUserMode();
        ShowUserModeState();
      } else if (message.wParam == kExitHotkeyId) {
        PostQuitMessage(0);
      }
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
