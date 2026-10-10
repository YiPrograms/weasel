#pragma once

#include <windows.h>
#include <algorithm>
#include <string>

// A visual composition hint; it never edits the target application's document.
// The actual commit is still delivered exclusively by the Rime IPC frontend.
enum class PreeditMode : DWORD { Off = 0, Auto = 1, Transparent = 2, Solid = 3 };

class PreeditOverlay {
 public:
  bool Create(HINSTANCE instance) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"WeaselUserModePreeditOverlay";
    wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
      return false;
    hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW |
            WS_EX_TRANSPARENT | WS_EX_LAYERED,
        wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1,
        nullptr, nullptr, instance, this);
    return hwnd_ != nullptr;
  }

  void Destroy() {
    if (hwnd_)
      DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }

  void Hide() {
    if (hwnd_ && visible_) {
      ShowWindow(hwnd_, SW_HIDE);
      visible_ = false;
    }
    text_.clear();
  }

  // Called on the frontend's GUI thread, never from the keyboard hook.
  void Update(const std::wstring& text, const RECT& caret, PreeditMode mode) {
    if (!hwnd_ || mode == PreeditMode::Off || text.empty() ||
        caret.right < caret.left || caret.bottom <= caret.top) {
      Hide();
      return;
    }
    bool dark = dark_background_;
    if (mode == PreeditMode::Auto) {
      // Sample just OUTSIDE the popup so it never samples its own pixels.
      // Hysteresis avoids flickering when the caret moves over antialiased text.
      HDC screen = GetDC(nullptr);
      if (screen) {
        COLORREF color = GetPixel(screen, caret.left - 4,
                                 caret.top + (caret.bottom - caret.top) / 2);
        ReleaseDC(nullptr, screen);
        if (color != CLR_INVALID) {
          const int brightness = (299 * GetRValue(color) +
                                  587 * GetGValue(color) +
                                  114 * GetBValue(color)) / 1000;
          if (brightness < 135) dark = true;
          if (brightness > 165) dark = false;
        }
      }
    } else if (mode == PreeditMode::Transparent) {
      // Retain the previous contrast decision for transparent mode.
    } else {
      dark = false;  // Solid mode always uses a readable white card.
    }

    HDC dc = GetDC(hwnd_);
    if (!dc) return;
    HFONT font = MakeFont(dc);
    HGDIOBJ previous = SelectObject(dc, font);
    SIZE size = {};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    TEXTMETRICW metrics = {};
    GetTextMetricsW(dc, &metrics);
    SelectObject(dc, previous);
    DeleteObject(font);
    ReleaseDC(hwnd_, dc);

    const int pad_x = mode == PreeditMode::Transparent ? 2 : 6;
    const int pad_y = mode == PreeditMode::Transparent ? 2 : 3;
    const int width = (std::min)(640, (std::max)(18, size.cx + pad_x * 2));
    const int height = (std::max)(18, metrics.tmHeight + pad_y * 2 + 2);
    const int left = caret.left;
    const int top = caret.top;
    const bool changed = !visible_ || text != text_ || mode != mode_ ||
                         dark != dark_background_ || left != left_ ||
                         top != top_ || width != width_ || height != height_;
    if (!changed) return;

    text_ = text;
    mode_ = mode;
    dark_background_ = dark;
    left_ = left; top_ = top; width_ = width; height_ = height;
    if (mode == PreeditMode::Transparent)
      SetLayeredWindowAttributes(hwnd_, RGB(255, 0, 255), 255, LWA_COLORKEY);
    else
      SetLayeredWindowAttributes(hwnd_, 0,
                                mode == PreeditMode::Auto ? 230 : 248,
                                LWA_ALPHA);
    SetWindowPos(hwnd_, HWND_TOPMOST, left, top, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    visible_ = true;
    InvalidateRect(hwnd_, nullptr, TRUE);
  }

 private:
  static HFONT MakeFont(HDC dc) {
    const int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    return CreateFontW(-MulDiv(20, dpi > 0 ? dpi : 96, 72), 0, 0, 0,
                       FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Noto Sans TC");
  }

  void Paint() {
    PAINTSTRUCT ps = {};
    HDC dc = BeginPaint(hwnd_, &ps);
    if (!dc) return;
    const bool transparent = mode_ == PreeditMode::Transparent;
    const COLORREF background =
        transparent ? RGB(255, 0, 255)
                    : (dark_background_ && mode_ == PreeditMode::Auto
                           ? RGB(38, 41, 48) : RGB(255, 255, 255));
    const COLORREF foreground = dark_background_ && mode_ != PreeditMode::Solid
                                     ? RGB(255, 255, 255) : RGB(15, 15, 15);
    RECT full = {0, 0, width_, height_};
    HBRUSH brush = CreateSolidBrush(background);
    FillRect(dc, &full, brush);
    DeleteObject(brush);
    HFONT font = MakeFont(dc);
    HGDIOBJ old_font = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    const int pad_x = transparent ? 2 : 6;
    const int pad_y = transparent ? 2 : 3;
    RECT text_rect = {pad_x, pad_y, width_ - pad_x, height_ - pad_y};
    if (transparent) {
      // One-pixel contrasting outline remains visible on mixed backgrounds.
      SetTextColor(dc, dark_background_ ? RGB(25, 25, 25) : RGB(255, 255, 255));
      RECT outline = text_rect;
      OffsetRect(&outline, 1, 1);
      DrawTextW(dc, text_.c_str(), -1, &outline,
                DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    SetTextColor(dc, foreground);
    DrawTextW(dc, text_.c_str(), -1, &text_rect,
              DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    // Composition underline differentiates this from committed application text.
    HPEN pen = CreatePen(PS_SOLID, 1, foreground);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    MoveToEx(dc, pad_x, height_ - 2, nullptr);
    LineTo(dc, width_ - pad_x, height_ - 2);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
    SelectObject(dc, old_font);
    DeleteObject(font);
    EndPaint(hwnd_, &ps);
  }

  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wp,
                                     LPARAM lp) {
    auto* self = reinterpret_cast<PreeditOverlay*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      auto* data = reinterpret_cast<CREATESTRUCTW*>(lp);
      self = static_cast<PreeditOverlay*>(data->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT && self) {
      self->Paint();
      return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
  }

  HWND hwnd_ = nullptr;
  std::wstring text_;
  PreeditMode mode_ = PreeditMode::Off;
  bool dark_background_ = false;
  bool visible_ = false;
  int left_ = 0, top_ = 0, width_ = 0, height_ = 0;
};
