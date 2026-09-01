#include "text_render.h"

#include "types.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <cwchar>
#include <algorithm>

namespace ustb {
namespace {

using Microsoft::WRL::ComPtr;

ComPtr<ID2D1Factory> g_d2d;
ComPtr<IDWriteFactory> g_dwrite;
ComPtr<IDWriteTextFormat> g_format;
ComPtr<ID2D1DCRenderTarget> g_dc_rt;
int g_dpi = 96;
bool g_drawing = false;

wchar_t g_face[LF_FACESIZE] = L"Segoe UI";
float g_size_dip = kFallbackTaskbarFontDip;
unsigned g_user_font_dip = kTaskbarFontDipAuto;
DWRITE_FONT_WEIGHT g_weight = DWRITE_FONT_WEIGHT_NORMAL;
DWRITE_FONT_STYLE g_style = DWRITE_FONT_STYLE_NORMAL;

D2D1_RECT_F to_dip(const RECT& rc) {
  const float scale = 96.0f / static_cast<float>(g_dpi);
  return D2D1::RectF(static_cast<float>(rc.left) * scale,
                     static_cast<float>(rc.top) * scale,
                     static_cast<float>(rc.right) * scale,
                     static_cast<float>(rc.bottom) * scale);
}

// Prefer the same Latin UI face as the Win11 tray clock. Shell status font on
// Chinese Windows is often Microsoft YaHei UI, which paints noticeably larger
// than Segoe UI Variable at the same nominal size.
bool pick_clock_face(wchar_t face[LF_FACESIZE]) {
  static const wchar_t* kCandidates[] = {
      L"Segoe UI Variable Text",
      L"Segoe UI Variable",
      L"Segoe UI",
  };
  if (!g_dwrite) {
    wcsncpy_s(face, LF_FACESIZE, L"Segoe UI", _TRUNCATE);
    return true;
  }
  ComPtr<IDWriteFontCollection> collection;
  if (FAILED(g_dwrite->GetSystemFontCollection(&collection)) || !collection) {
    wcsncpy_s(face, LF_FACESIZE, L"Segoe UI", _TRUNCATE);
    return true;
  }
  for (const wchar_t* name : kCandidates) {
    UINT32 index = 0;
    BOOL exists = FALSE;
    if (SUCCEEDED(collection->FindFamilyName(name, &index, &exists)) &&
        exists) {
      wcsncpy_s(face, LF_FACESIZE, name, _TRUNCATE);
      return true;
    }
  }
  wcsncpy_s(face, LF_FACESIZE, L"Segoe UI", _TRUNCATE);
  return true;
}

// SPI_GETNONCLIENTMETRICS returns LOGFONT at 96 DPI (DIP) units. That maps
// directly to DirectWrite's fontSize, which is also in DIPs. Using
// SystemParametersInfoForDpi is avoided — it has known system-wide font bugs.
// Size tracks the shell font (accessibility text scaling) but is scaled down to
// match the tray clock/date, which is smaller than lfStatusFont.
bool read_shell_font(wchar_t face[LF_FACESIZE], float* size_dip,
                     DWRITE_FONT_WEIGHT* weight, DWRITE_FONT_STYLE* style) {
  NONCLIENTMETRICSW ncm{};
  ncm.cbSize = sizeof(ncm);
  if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
    return false;
  }
  const LOGFONTW& lf = ncm.lfStatusFont;
  int px = lf.lfHeight;
  if (px < 0) {
    px = -px;
  }
  if (px <= 0) {
    px = 12;
  }
  *size_dip = static_cast<float>(px) * kShellToClockFontScale;
  if (*size_dip < 8.0f) {
    *size_dip = 8.0f;
  }
  pick_clock_face(face);
  *weight = DWRITE_FONT_WEIGHT_NORMAL;
  *style = DWRITE_FONT_STYLE_NORMAL;
  (void)lf;
  return true;
}

bool ensure_format() {
  if (!g_dwrite) {
    return false;
  }
  if (g_format) {
    return true;
  }
  wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
  if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) == 0) {
    wcscpy_s(locale, L"en-us");
  }
  const HRESULT hr = g_dwrite->CreateTextFormat(
      g_face, nullptr, g_weight, g_style, DWRITE_FONT_STRETCH_NORMAL,
      g_size_dip, locale, &g_format);
  if (FAILED(hr)) {
    // Face missing (rare) — fall back to Segoe UI at the same size.
    const HRESULT hr2 = g_dwrite->CreateTextFormat(
        L"Segoe UI", nullptr, g_weight, g_style, DWRITE_FONT_STRETCH_NORMAL,
        g_size_dip, locale, &g_format);
    if (FAILED(hr2)) {
      return false;
    }
  }
  g_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  return true;
}

void apply_font_params() {
  wchar_t face[LF_FACESIZE]{};
  float size = kFallbackTaskbarFontDip;
  DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
  DWRITE_FONT_STYLE style = DWRITE_FONT_STYLE_NORMAL;
  if (!read_shell_font(face, &size, &weight, &style)) {
    wcsncpy_s(face, L"Segoe UI", _TRUNCATE);
  }
  if (g_user_font_dip > kTaskbarFontDipAuto) {
    size = static_cast<float>(g_user_font_dip);
  }
  if (g_format && wcscmp(face, g_face) == 0 && size == g_size_dip &&
      weight == g_weight && style == g_style) {
    return;
  }
  wcsncpy_s(g_face, face, _TRUNCATE);
  g_size_dip = size;
  g_weight = weight;
  g_style = style;
  g_format.Reset();
  ensure_format();
}

void refresh_shell_font_params() { apply_font_params(); }

bool ensure_dc_rt() {
  if (!g_d2d || !ensure_format()) {
    return false;
  }
  if (g_dc_rt) {
    return true;
  }
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_DEFAULT,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                        D2D1_ALPHA_MODE_PREMULTIPLIED),
      static_cast<float>(g_dpi), static_cast<float>(g_dpi));
  const HRESULT hr = g_d2d->CreateDCRenderTarget(&props, &g_dc_rt);
  return SUCCEEDED(hr);
}

ComPtr<IDWriteTextLayout> make_layout(const wchar_t* text, bool tabular) {
  (void)tabular;
  ComPtr<IDWriteTextLayout> layout;
  if (!g_dwrite || !g_format || text == nullptr) {
    return layout;
  }
  const size_t len = wcslen(text);
  if (len == 0) {
    return layout;
  }
  HRESULT hr = g_dwrite->CreateTextLayout(text, static_cast<UINT32>(len),
                                          g_format.Get(), 4096.0f, 4096.0f,
                                          &layout);
  if (FAILED(hr)) {
    layout.Reset();
    return layout;
  }
  return layout;
}

}  // namespace

bool text_render_init() {
  if (g_d2d && g_dwrite) {
    return ensure_format();
  }
  HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                 g_d2d.GetAddressOf());
  if (FAILED(hr)) {
    return false;
  }
  hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                           reinterpret_cast<IUnknown**>(
                               g_dwrite.ReleaseAndGetAddressOf()));
  if (FAILED(hr)) {
    text_render_shutdown();
    return false;
  }
  refresh_shell_font_params();
  return ensure_format() && ensure_dc_rt();
}

void text_render_shutdown() {
  g_drawing = false;
  g_dc_rt.Reset();
  g_format.Reset();
  g_dwrite.Reset();
  g_d2d.Reset();
  g_dpi = 96;
  g_size_dip = kFallbackTaskbarFontDip;
  g_user_font_dip = kTaskbarFontDipAuto;
  g_weight = DWRITE_FONT_WEIGHT_NORMAL;
  g_style = DWRITE_FONT_STYLE_NORMAL;
  wcscpy_s(g_face, L"Segoe UI");
}

void text_render_set_dpi(int dpi) {
  if (dpi <= 0) {
    dpi = 96;
  }
  if (g_dpi == dpi && g_dc_rt) {
    return;
  }
  g_dpi = dpi;
  // Font size is in DIPs (DPI-independent); only the DC target needs rebuild.
  g_dc_rt.Reset();
  ensure_format();
  ensure_dc_rt();
}

void text_render_reload_system_font() { apply_font_params(); }

void text_render_set_font_dip(unsigned dip) {
  if (dip > kTaskbarFontDipAuto) {
    dip = std::clamp(dip, kMinTaskbarFontDip, kMaxTaskbarFontDip);
  } else {
    dip = kTaskbarFontDipAuto;
  }
  if (g_user_font_dip == dip && g_format) {
    return;
  }
  g_user_font_dip = dip;
  apply_font_params();
}

int text_render_width(const wchar_t* text, bool tabular) {
  if (!ensure_format()) {
    return 0;
  }
  const ComPtr<IDWriteTextLayout> layout = make_layout(text, tabular);
  if (!layout) {
    return 0;
  }
  DWRITE_TEXT_METRICS metrics{};
  if (FAILED(layout->GetMetrics(&metrics))) {
    return 0;
  }
  return static_cast<int>(metrics.widthIncludingTrailingWhitespace *
                              static_cast<float>(g_dpi) / 96.0f +
                          0.5f);
}

int text_render_line_height() {
  if (!ensure_format()) {
    return 0;
  }
  const auto fallback_px = [&]() {
    return static_cast<int>(g_size_dip * static_cast<float>(g_dpi) / 96.0f +
                            0.5f);
  };
  ComPtr<IDWriteFontCollection> collection;
  if (FAILED(g_format->GetFontCollection(&collection)) || !collection) {
    return fallback_px();
  }
  wchar_t family_name[LF_FACESIZE]{};
  if (FAILED(g_format->GetFontFamilyName(family_name, LF_FACESIZE)) ||
      family_name[0] == L'\0') {
    wcsncpy_s(family_name, g_face, _TRUNCATE);
  }
  UINT32 index = 0;
  BOOL exists = FALSE;
  if (FAILED(collection->FindFamilyName(family_name, &index, &exists)) ||
      !exists) {
    return fallback_px();
  }
  ComPtr<IDWriteFontFamily> family;
  if (FAILED(collection->GetFontFamily(index, &family)) || !family) {
    return 0;
  }
  ComPtr<IDWriteFont> font;
  if (FAILED(family->GetFirstMatchingFont(
          g_format->GetFontWeight(), g_format->GetFontStretch(),
          g_format->GetFontStyle(), &font)) ||
      !font) {
    return 0;
  }
  DWRITE_FONT_METRICS metrics{};
  font->GetMetrics(&metrics);
  if (metrics.designUnitsPerEm == 0) {
    return 0;
  }
  const float dip =
      g_size_dip *
      static_cast<float>(metrics.ascent + metrics.descent + metrics.lineGap) /
      static_cast<float>(metrics.designUnitsPerEm);
  return static_cast<int>(dip * static_cast<float>(g_dpi) / 96.0f + 0.5f);
}

bool text_render_begin(HDC hdc, const RECT& bounds) {
  if (!ensure_dc_rt() || g_drawing) {
    return false;
  }
  g_dc_rt->BindDC(hdc, &bounds);
  g_dc_rt->SetDpi(static_cast<float>(g_dpi), static_cast<float>(g_dpi));
  g_dc_rt->BeginDraw();
  g_dc_rt->Clear(D2D1::ColorF(0, 0, 0, 0));
  g_drawing = true;
  return true;
}

void text_render_fill_rounded_rect(const RECT& rc, float radius_px, COLORREF rgb,
                                   float alpha) {
  if (!g_drawing || alpha <= 0.0f) {
    return;
  }
  const D2D1_RECT_F dip = to_dip(rc);
  const float radius = radius_px * 96.0f / static_cast<float>(g_dpi);
  ComPtr<ID2D1SolidColorBrush> brush;
  if (FAILED(g_dc_rt->CreateSolidColorBrush(
          D2D1::ColorF(GetRValue(rgb) / 255.0f, GetGValue(rgb) / 255.0f,
                       GetBValue(rgb) / 255.0f, alpha),
          &brush))) {
    return;
  }
  g_dc_rt->FillRoundedRectangle(
      D2D1::RoundedRect(dip, radius, radius), brush.Get());
}

void text_render_draw_rounded_rect(const RECT& rc, float radius_px, COLORREF rgb,
                                   float alpha, float stroke_px) {
  if (!g_drawing || alpha <= 0.0f || stroke_px <= 0.0f) {
    return;
  }
  const D2D1_RECT_F dip = to_dip(rc);
  const float scale = 96.0f / static_cast<float>(g_dpi);
  const float radius = radius_px * scale;
  const float stroke = stroke_px * scale;
  ComPtr<ID2D1SolidColorBrush> brush;
  if (FAILED(g_dc_rt->CreateSolidColorBrush(
          D2D1::ColorF(GetRValue(rgb) / 255.0f, GetGValue(rgb) / 255.0f,
                       GetBValue(rgb) / 255.0f, alpha),
          &brush))) {
    return;
  }
  g_dc_rt->DrawRoundedRectangle(D2D1::RoundedRect(dip, radius, radius),
                                brush.Get(), stroke);
}

void text_render_draw(const wchar_t* text, const RECT& rc, COLORREF fg,
                      bool center, bool tabular) {
  if (!g_drawing || !text || text[0] == L'\0') {
    return;
  }

  const ComPtr<IDWriteTextLayout> layout = make_layout(text, tabular);
  if (!layout) {
    return;
  }

  layout->SetTextAlignment(center ? DWRITE_TEXT_ALIGNMENT_CENTER
                                  : DWRITE_TEXT_ALIGNMENT_LEADING);
  layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

  const D2D1_RECT_F dip = to_dip(rc);
  layout->SetMaxWidth(dip.right - dip.left);
  layout->SetMaxHeight(dip.bottom - dip.top);

  ComPtr<ID2D1SolidColorBrush> brush;
  const float r = GetRValue(fg) / 255.0f;
  const float g = GetGValue(fg) / 255.0f;
  const float b = GetBValue(fg) / 255.0f;
  if (FAILED(g_dc_rt->CreateSolidColorBrush(D2D1::ColorF(r, g, b, 1.0f),
                                            &brush))) {
    return;
  }
  g_dc_rt->DrawTextLayout(D2D1::Point2F(dip.left, dip.top), layout.Get(),
                          brush.Get());
}

bool text_render_end() {
  if (!g_drawing) {
    return false;
  }
  g_drawing = false;
  return SUCCEEDED(g_dc_rt->EndDraw());
}

}  // namespace ustb
