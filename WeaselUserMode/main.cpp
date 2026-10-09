#include <windows.h>
#include <imm.h>
#include <msctf.h>

#include <array>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

#include <KeyEvent.h>
#include <ResponseParser.h>
#include <WeaselIPC.h>

namespace {

HHOOK g_keyboard_hook = nullptr;
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

bool DrainResponse(std::wstring* commit = nullptr) {
  weasel::ResponseParser parser(commit);
  return UserModeClient().GetResponseData(std::ref(parser));
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

bool ForegroundUsesPlainKeyboardLayout() {
  const HWND foreground = GetForegroundWindow();
  if (!foreground)
    return false;

  const DWORD thread_id = GetWindowThreadProcessId(foreground, nullptr);
  if (!thread_id)
    return false;

  const HKL layout = GetKeyboardLayout(thread_id);
  if (!layout)
    return false;

  // Decide from the foreground thread's actual HKL. A process-wide TSF
  // GetActiveProfile() query can describe a different app when Windows keeps
  // per-app input methods, which caused false suspension of User Mode.
  if (ImmIsIME(layout))
    return false;

  return true;
}

void StopInterception() {
  if (!g_intercepting)
    return;
  UserModeClient().ClearComposition();
  DrainResponse();
  UserModeClient().FocusOut();
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
          L"Weasel User Mode: ARMED\n\nIt is enabled, but the current "
          L"foreground app is using another Windows IME/layout. Switch to a "
          L"plain keyboard layout such as English (US).";
      break;
  }
  MessageBoxW(nullptr, message, L"Weasel User Mode",
              MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
}

bool RefreshForegroundSession() {
  const HWND foreground = GetForegroundWindow();
  if (foreground != g_foreground_window) {
    StopInterception();
    g_foreground_window = foreground;
  }

  const bool should_intercept = g_user_enabled && g_foreground_window &&
                                ForegroundUsesPlainKeyboardLayout();
  if (!should_intercept) {
    StopInterception();
    return false;
  }

  if (!g_intercepting) {
    UserModeClient().FocusIn();
    g_intercepting = true;
  }
  if (g_foreground_window)
    UserModeClient().UpdateInputPosition(GetInputPosition());
  return true;
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
  SetKeyStateForEvent(state, hook.vkCode, key_up);
  KeyInfo info(static_cast<LPARAM>(lparam));
  return ConvertKeyEvent(hook.vkCode, info, state.data(), result);
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

LRESULT CALLBACK KeyboardHook(int code, WPARAM wparam, LPARAM lparam) {
  if (code < 0)
    return CallNextHookEx(g_keyboard_hook, code, wparam, lparam);

  const auto& hook = *reinterpret_cast<KBDLLHOOKSTRUCT*>(lparam);
  if (hook.flags & LLKHF_INJECTED)
    return CallNextHookEx(g_keyboard_hook, code, wparam, lparam);

  const bool key_down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
  const bool key_up = wparam == WM_KEYUP || wparam == WM_SYSKEYUP;
  if (!key_down && !key_up)
    return CallNextHookEx(g_keyboard_hook, code, wparam, lparam);

  bool swallow_release = false;
  if (key_up && hook.vkCode < g_swallowed_keys.size() &&
      g_swallowed_keys[hook.vkCode]) {
    swallow_release = true;
    g_swallowed_keys[hook.vkCode] = false;
  }

  // If we swallowed the matching key-down, its key-up must also stay hidden
  // even if a modifier was pressed in between.
  if (swallow_release)
    return 1;

  if (ShouldBypassSystemShortcut(hook))
    return CallNextHookEx(g_keyboard_hook, code, wparam, lparam);

  // Never reconnect, launch a process or reposition the UI from the hook.
  // Those operations perform blocking IPC and can make Windows silently
  // remove a low-level hook after its one-second timeout.
  if (!g_intercepting || !g_user_enabled ||
      GetForegroundWindow() != g_foreground_window ||
      !ForegroundUsesPlainKeyboardLayout())
    return CallNextHookEx(g_keyboard_hook, code, wparam, lparam);

  weasel::KeyEvent key_event;
  if (!ConvertLowLevelKey(hook, key_up, key_event))
    return CallNextHookEx(g_keyboard_hook, code, wparam, lparam);

  const bool handled = UserModeClient().ProcessKeyEvent(key_event);
  std::wstring commit;
  DrainResponse(&commit);
  if (!commit.empty())
    SendUnicode(commit);

  if (key_down && handled && hook.vkCode < g_swallowed_keys.size())
    g_swallowed_keys[hook.vkCode] = true;

  return handled ? 1 : CallNextHookEx(g_keyboard_hook, code, wparam, lparam);
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
  const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool com_initialized = SUCCEEDED(com_result);
  if (com_initialized) {
    CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                     CLSCTX_INPROC_SERVER, IID_ITfInputProcessorProfileMgr,
                     reinterpret_cast<void**>(&g_profile_manager));
  }

  if (!ConnectServer()) {
    MessageBoxW(
        nullptr,
        L"Could not connect to WeaselServer.exe. Keep WeaselUserMode.exe "
        L"next to the normal Weasel runtime files.",
        L"Weasel User Mode", MB_OK | MB_ICONERROR);
    if (g_profile_manager)
      g_profile_manager->Release();
    if (com_initialized)
      CoUninitialize();
    return 2;
  }

  g_user_enabled = ReadUserModeDword(L"StartEnabled", 1) != 0;
  g_foreground_window = GetForegroundWindow();
  RefreshForegroundSession();

  const bool toggle_hotkey_registered = RegisterToggleHotkey();
  RegisterHotKey(nullptr, kExitHotkeyId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT,
                 VK_F12);
  const UINT_PTR input_mode_timer = SetTimer(nullptr, 1, 250, nullptr);

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
    UserModeClient().Disconnect();
    if (g_profile_manager)
      g_profile_manager->Release();
    if (com_initialized)
      CoUninitialize();
    return 3;
  }

  MSG message = {};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (message.message == WM_HOTKEY) {
      if (message.wParam == kToggleHotkeyId) {
        g_user_enabled = !g_user_enabled;
        if (!g_user_enabled)
          StopInterception();
        else
          RefreshForegroundSession();
        ShowUserModeState();
      } else if (message.wParam == kExitHotkeyId) {
        PostQuitMessage(0);
      }
      continue;
    }
    if (message.message == WM_TIMER && message.wParam == input_mode_timer) {
      if (ConnectServer())
        RefreshForegroundSession();
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
  UserModeClient().EndSession();
  UserModeClient().Disconnect();
  if (g_profile_manager) {
    g_profile_manager->Release();
    g_profile_manager = nullptr;
  }
  if (com_initialized)
    CoUninitialize();
  return 0;
}
