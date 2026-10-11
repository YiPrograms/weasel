#pragma once

#include <windows.h>
#include <algorithm>
#include <string>
#include <vector>

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
    if (mode == PreeditMode::Auto || mode == PreeditMode::Transparent) {
      // A single pixel immediately to the left of the caret can be dark
      // text, an I-beam, or the candidate border on an otherwise white
      // editor. Take the median of several nearby pixels, mostly above or
      // to the right of the preedit. This also avoids classifying our own
      // popup as the underlying application background.
      HDC screen = GetDC(nullptr);
      if (screen) {
        const POINT sample_points[] = {
            {caret.left + 80, caret.top - 8},
            {caret.left + 130, caret.top - 8},
            {caret.left + 210, caret.top - 8},
            {caret.left + 80, caret.top + 4},
            {caret.left + 140, caret.top + 4},
            {caret.left + 220, caret.top + 4},
            {caret.left - 10, caret.top - 6}};
        int brightnesses[sizeof(sample_points) / sizeof(sample_points[0])];
        size_t valid = 0;
        for (const POINT& point : sample_points) {
          const COLORREF color = GetPixel(screen, point.x, point.y);
          if (color == CLR_INVALID) continue;
          brightnesses[valid++] = (299 * GetRValue(color) +
                                   587 * GetGValue(color) +
                                   114 * GetBValue(color)) / 1000;
        }
        ReleaseDC(nullptr, screen);
        if (valid != 0) {
          std::sort(brightnesses, brightnesses + valid);
          // Hysteresis prevents flashing while moving past text or borders.
          const int brightness = brightnesses[valid / 2];
          if (brightness < 135) dark = true;
          if (brightness > 165) dark = false;
        }
      }
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
    const int width = (std::min)(640, (std::max)(18,
        static_cast<int>(size.cx) + pad_x * 2));
    const int height = (std::max)(18,
        static_cast<int>(metrics.tmHeight) + pad_y * 2 + 2);
    const int left = caret.left;
    const int top = caret.top;
    const bool changed = !visible_ || text != text_ || mode != mode_ ||
                         dark != dark_background_ || left != left_ ||
                         top != top_ || width != width_ || height != height_;
    if (!changed) {
      // Weasel's candidate panel is itself topmost. It can be recreated or
      // raised after this overlay, covering the composition at the caret.
      // Reassert z-order while composing; do not activate or move focus.
      SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      return;
    }

    const bool mode_changed = mode != mode_;
    text_ = text;
    mode_ = mode;
    dark_background_ = dark;
    left_ = left; top_ = top; width_ = width; height_ = height;
    if (mode_changed) {
      // A prior SetLayeredWindowAttributes call prevents subsequent
      // UpdateLayeredWindow calls until WS_EX_LAYERED has been reset.
      const LONG_PTR style = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
      SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, style & ~WS_EX_LAYERED);
      SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, style);
    }
    if (mode == PreeditMode::Transparent) {
      if (!PaintTransparent()) Hide();
      else visible_ = true;
      return;
    }
    SetLayeredWindowAttributes(hwnd_, 0,
                               mode == PreeditMode::Auto ? 230 : 248,
                               LWA_ALPHA);
    SetWindowPos(hwnd_, HWND_TOPMOST, left, top, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    visible_ = true;
    InvalidateRect(hwnd_, nullptr, TRUE);
  }

 private:
  // A magenta color key leaves brightly colored antialiasing fringes around
  // glyphs. Render a grayscale mask into a DIB and upload premultiplied BGRA
  // instead so transparent preedit has smooth, genuinely alpha-blended edges.
  bool PaintTransparent() {
    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    HDC memory = CreateCompatibleDC(screen);
    if (!memory) {
      ReleaseDC(nullptr, screen);
      return false;
    }
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width_;
    info.bmiHeader.biHeight = -height_;  // Top-down BGRA pixels.
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS,
                                      &pixels, nullptr, 0);
    if (!bitmap || !pixels) {
      if (bitmap) DeleteObject(bitmap);
      DeleteDC(memory);
      ReleaseDC(nullptr, screen);
      return false;
    }
    HGDIOBJ old_bitmap = SelectObject(memory, bitmap);
    const size_t count = static_cast<size_t>(width_) * height_;
    auto* bgra = static_cast<BYTE*>(pixels);
    std::fill(bgra, bgra + count * 4, 0);
    HFONT font = MakeFont(memory, true);
    HGDIOBJ old_font = SelectObject(memory, font);
    SetBkMode(memory, TRANSPARENT);
    SetTextColor(memory, RGB(255, 255, 255));
    const RECT text_rect = {2, 2, width_ - 2, height_ - 2};
    RECT bounds = text_rect;
    DrawTextW(memory, text_.c_str(), -1, &bounds,
              DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
    HGDIOBJ old_pen = SelectObject(memory, pen);
    MoveToEx(memory, 2, height_ - 2, nullptr);
    LineTo(memory, width_ - 2, height_ - 2);
    SelectObject(memory, old_pen);
    DeleteObject(pen);
    SelectObject(memory, old_font);
    DeleteObject(font);

    std::vector<BYTE> mask(count);
    for (size_t i = 0; i < count; ++i)
      mask[i] = (std::max)({bgra[i * 4], bgra[i * 4 + 1],
                           bgra[i * 4 + 2]});
    const int ink = dark_background_ ? 250 : 15;
    const int rim = dark_background_ ? 0 : 255;
    for (int y = 0; y < height_; ++y) {
      for (int x = 0; x < width_; ++x) {
        const size_t i = static_cast<size_t>(y) * width_ + x;
        const int alpha = mask[i];
        int nearby = alpha;
        for (int yy = (std::max)(0, y - 1);
             yy <= (std::min)(height_ - 1, y + 1); ++yy)
          for (int xx = (std::max)(0, x - 1);
               xx <= (std::min)(width_ - 1, x + 1); ++xx)
            nearby = (std::max)(nearby, static_cast<int>(
                mask[static_cast<size_t>(yy) * width_ + xx]));
        const int edge = ((nearby - alpha) * (255 - alpha)) / (255 * 3);
        const int combined = (std::min)(255, alpha + edge);
        const BYTE channel = static_cast<BYTE>(
            (ink * alpha + rim * edge + 127) / 255);
        bgra[i * 4] = channel;
        bgra[i * 4 + 1] = channel;
        bgra[i * 4 + 2] = channel;
        bgra[i * 4 + 3] = static_cast<BYTE>(combined);
      }
    }
    POINT origin = {left_, top_};
    SIZE size = {width_, height_};
    POINT src = {0, 0};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    SetWindowPos(hwnd_, HWND_TOPMOST, left_, top_, width_, height_,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOREDRAW);
    const BOOL updated = UpdateLayeredWindow(hwnd_, screen, &origin, &size,
                                              memory, &src, 0, &blend,
                                              ULW_ALPHA);
    SelectObject(memory, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return updated != FALSE;
  }

  static HFONT MakeFont(HDC dc, bool grayscale = false) {
    const int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    return CreateFontW(-MulDiv(20, dpi > 0 ? dpi : 96, 72), 0, 0, 0,
                       FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       grayscale ? ANTIALIASED_QUALITY : CLEARTYPE_QUALITY,
                       DEFAULT_PITCH, L"Noto Sans TC");
  }

  void Paint() {
    PAINTSTRUCT ps = {};
    HDC dc = BeginPaint(hwnd_, &ps);
    if (!dc) return;
    if (mode_ == PreeditMode::Transparent) {
      EndPaint(hwnd_, &ps);
      return;  // UpdateLayeredWindow owns these pixels, not WM_PAINT.
    }
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
