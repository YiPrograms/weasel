#pragma once
#include <windows.h>
#include <shellapi.h>

#include <array>
#include <functional>
#include <string>
#include <utility>

enum class TrayMode { Chinese, English, Paused, Off, Error };

class TrayStatus {
 public:
  using Callback = std::function<void()>;
  static constexpr wchar_t kWindowClass[] = L"WeaselUserModeFrontendWindow";
  static constexpr UINT kActivateMessage = WM_APP + 52;
  static constexpr UINT kTrayMessage = WM_APP + 51;

  bool Create(HINSTANCE instance,
              Callback toggle,
              Callback settings,
              Callback deploy,
              Callback quit) {
    toggle_ = std::move(toggle);
    settings_ = std::move(settings);
    deploy_ = std::move(deploy);
    quit_ = std::move(quit);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
      return false;
    hwnd_ = CreateWindowExW(0, kWindowClass, L"Weasel User Mode", WS_OVERLAPPED,
                            0, 0, 0, 0, nullptr, nullptr, instance, this);
    if (!hwnd_)
      return false;
    taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
    icons_ = {
        MakeIcon(L"中", RGB(32, 142, 88)), MakeIcon(L"英", RGB(43, 115, 181)),
        MakeIcon(L"停", RGB(186, 132, 28)), MakeIcon(L"關", RGB(105, 105, 105)),
        MakeIcon(L"!", RGB(180, 48, 48))};
    Update(TrayMode::Paused, L"Rime 正在初始化");
    return true;
  }

  void Update(TrayMode mode, const std::wstring& reason) {
    if (!hwnd_)
      return;
    if (mode == mode_ && reason == reason_ && added_)
      return;
    mode_ = mode;
    reason_ = reason;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = kTrayMessage;
    nid.hIcon = icons_[static_cast<size_t>(mode_)];
    std::wstring title = std::wstring(L"Weasel User Mode - ") + Label(mode_);
    if (!reason_.empty()) {
      title += L" | ";
      title += reason_;
    }
    wcsncpy_s(nid.szTip, title.c_str(), _TRUNCATE);
    if (!Shell_NotifyIconW(added_ ? NIM_MODIFY : NIM_ADD, &nid)) {
      added_ = false;
      return;
    }
    added_ = true;
    nid.uFlags = 0;
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
  }

  void Notice(const std::wstring& message) {
    if (!added_)
      return;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    wcsncpy_s(nid.szInfoTitle, L"Weasel User Mode", _TRUNCATE);
    wcsncpy_s(nid.szInfo, message.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
  }

  void Destroy() {
    if (added_) {
      NOTIFYICONDATAW nid = {};
      nid.cbSize = sizeof(nid);
      nid.hWnd = hwnd_;
      nid.uID = 1;
      Shell_NotifyIconW(NIM_DELETE, &nid);
      added_ = false;
    }
    if (hwnd_)
      DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    for (HICON& icon : icons_) {
      if (icon)
        DestroyIcon(icon);
      icon = nullptr;
    }
  }

  HWND hwnd() const { return hwnd_; }

 private:
  static const wchar_t* Label(TrayMode state) {
    switch (state) {
      case TrayMode::Chinese:
        return L"中文";
      case TrayMode::English:
        return L"英文";
      case TrayMode::Paused:
        return L"暫停";
      case TrayMode::Off:
        return L"關閉";
      case TrayMode::Error:
        return L"服務異常";
    }
    return L"未知";
  }

  static HICON MakeIcon(const wchar_t* text, COLORREF background) {
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP color = CreateCompatibleBitmap(screen, 32, 32);
    std::array<BYTE, 128> opaque_mask{};
    HBITMAP mask = CreateBitmap(32, 32, 1, 1, opaque_mask.data());
    HGDIOBJ previous = SelectObject(dc, color);
    RECT rect = {0, 0, 32, 32};
    HBRUSH brush = CreateSolidBrush(background);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
    HFONT font =
        CreateFontW(-23, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    DEFAULT_PITCH, L"Microsoft YaHei UI");
    HGDIOBJ previous_font = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, previous_font);
    DeleteObject(font);
    SelectObject(dc, previous);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    ICONINFO icon_info = {};
    icon_info.fIcon = TRUE;
    icon_info.hbmColor = color;
    icon_info.hbmMask = mask;
    HICON icon = CreateIconIndirect(&icon_info);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
  }

  void ShowMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu)
      return;
    std::wstring name = std::wstring(L"目前狀態：") + Label(mode_);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, name.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 100,
                mode_ == TrayMode::Off ? L"啟用 User Mode" : L"停用 User Mode");
    AppendMenuW(menu, MF_STRING, 101, L"輸入法設定");
    AppendMenuW(menu, MF_STRING, 102, L"重新部署 Rime");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 103, L"結束 User Mode");
    POINT point = {};
    GetCursorPos(&point);
    SetForegroundWindow(hwnd_);
    const UINT action =
        TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                       point.x, point.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    if (action == 100 && toggle_)
      toggle_();
    if (action == 101 && settings_)
      settings_();
    if (action == 102 && deploy_)
      deploy_();
    if (action == 103 && quit_)
      quit_();
  }

  static LRESULT CALLBACK WindowProc(HWND hwnd,
                                     UINT msg,
                                     WPARAM wp,
                                     LPARAM lp) {
    auto* self =
        reinterpret_cast<TrayStatus*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
      auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
      self = static_cast<TrayStatus*>(create->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self)
      return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == kActivateMessage) {
      self->Notice(L"已經在執行；請使用系統匣圖示查看與切換狀態。");
      return 0;
    }
    if (msg == kTrayMessage) {
      const UINT event = LOWORD(lp);
      if ((event == WM_LBUTTONUP || event == NIN_SELECT) && self->toggle_)
        self->toggle_();
      else if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU)
        self->ShowMenu();
      return 0;
    }
    if (msg == self->taskbar_created_ && self->taskbar_created_) {
      self->added_ = false;
      self->Update(self->mode_, self->reason_);
      return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }

  HWND hwnd_ = nullptr;
  UINT taskbar_created_ = 0;
  bool added_ = false;
  TrayMode mode_ = TrayMode::Off;
  std::wstring reason_;
  std::array<HICON, 5> icons_{};
  Callback toggle_, settings_, deploy_, quit_;
};
