#include <windows.h>

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
weasel::Client g_client;
bool g_enabled = true;
HWND g_foreground_window = nullptr;
std::array<bool, 256> g_swallowed_keys{};
bool g_toggle_space_down = false;
bool g_exit_hotkey_down = false;

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

  const HINSTANCE result = ShellExecuteW(nullptr, L"open", server.c_str(),
                                         nullptr, directory.c_str(), SW_HIDE);
  return reinterpret_cast<INT_PTR>(result) > 32;
}

bool DrainResponse(std::wstring* commit = nullptr) {
  weasel::ResponseParser parser(commit);
  return g_client.GetResponseData(std::ref(parser));
}

bool ConnectServer() {
  if (g_client.Echo())
    return true;

  g_client.Disconnect();
  if (g_client.Connect()) {
    g_client.StartSession();
    DrainResponse();
    if (g_client.Echo())
      return true;
    g_client.Disconnect();
  }

  if (!LaunchServer())
    return false;

  for (int attempt = 0; attempt < 30; ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!g_client.Connect())
      continue;
    g_client.StartSession();
    DrainResponse();
    if (g_client.Echo())
      return true;
    g_client.Disconnect();
  }
  return false;
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

void UpdateForegroundSession() {
  const HWND foreground = GetForegroundWindow();
  if (foreground != g_foreground_window) {
    if (g_foreground_window)
      g_client.FocusOut();
    g_foreground_window = foreground;
    if (g_foreground_window)
      g_client.FocusIn();
  }
  if (g_foreground_window)
    g_client.UpdateInputPosition(GetInputPosition());
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

  // Prototype controls: Ctrl+Space toggles interception. Ctrl+Alt+F12 exits.
  if (key_down && hook.vkCode == VK_SPACE && IsPressed(VK_CONTROL) &&
      !g_toggle_space_down) {
    g_enabled = !g_enabled;
    if (!g_enabled) {
      g_client.ClearComposition();
      g_client.FocusOut();
      g_foreground_window = nullptr;
    }
    g_toggle_space_down = true;
    return 1;
  }
  if (key_down && hook.vkCode == VK_F12 && IsPressed(VK_CONTROL) &&
      IsPressed(VK_MENU) && !g_exit_hotkey_down) {
    PostQuitMessage(0);
    g_exit_hotkey_down = true;
    return 1;
  }
  if (key_up && hook.vkCode == VK_SPACE && g_toggle_space_down) {
    g_toggle_space_down = false;
    return 1;
  }
  if (key_up && hook.vkCode == VK_F12 && g_exit_hotkey_down) {
    g_exit_hotkey_down = false;
    return 1;
  }

  bool swallow_release = false;
  if (key_up && hook.vkCode < g_swallowed_keys.size() &&
      g_swallowed_keys[hook.vkCode]) {
    swallow_release = true;
    g_swallowed_keys[hook.vkCode] = false;
  }

  if (!g_enabled || !ConnectServer())
    return swallow_release
               ? 1
               : CallNextHookEx(g_keyboard_hook, code, wparam, lparam);

  UpdateForegroundSession();

  weasel::KeyEvent key_event;
  if (!ConvertLowLevelKey(hook, key_up, key_event))
    return CallNextHookEx(g_keyboard_hook, code, wparam, lparam);

  const bool handled = g_client.ProcessKeyEvent(key_event);
  std::wstring commit;
  DrainResponse(&commit);
  if (!commit.empty())
    SendUnicode(commit);

  if (key_down && handled && hook.vkCode < g_swallowed_keys.size())
    g_swallowed_keys[hook.vkCode] = true;

  return (handled || swallow_release)
             ? 1
             : CallNextHookEx(g_keyboard_hook, code, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  if (!ConnectServer()) {
    MessageBoxW(
        nullptr,
        L"Could not connect to WeaselServer.exe. Keep WeaselUserMode.exe "
        L"next to the normal Weasel runtime files.",
        L"Weasel User Mode", MB_OK | MB_ICONERROR);
    return 2;
  }

  g_client.FocusIn();
  g_foreground_window = GetForegroundWindow();
  g_client.UpdateInputPosition(GetInputPosition());

  g_keyboard_hook =
      SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHook, instance, 0);
  if (!g_keyboard_hook) {
    MessageBoxW(nullptr, L"Could not install the keyboard hook.",
                L"Weasel User Mode", MB_OK | MB_ICONERROR);
    g_client.FocusOut();
    g_client.Disconnect();
    return 3;
  }

  MSG message = {};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  UnhookWindowsHookEx(g_keyboard_hook);
  g_keyboard_hook = nullptr;
  g_client.FocusOut();
  g_client.EndSession();
  g_client.Disconnect();
  return 0;
}
