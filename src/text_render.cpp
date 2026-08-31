#include "text_render.h"

#include "types.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
namespace ustb {
namespace {

using Microsoft::WRL::ComPtr;

ComPtr<ID2D1Factory> g_d2d;
ComPtr<IDWriteFactory> g_dwrite;
ComPtr<IDWriteTextFormat> g_format;
ComPtr<ID2D1DCRenderTarget> g_dc_rt;
int g_dpi = 96;
bool g_drawing = false;

constexpr wchar_t kUIFont[] = L"Segoe UI";

float font_size_dip(int dpi) {
  return kTaskbarFontPt * static_cast<float>(dpi) / 72.0f;
}

D2D1_RECT_F to_dip(const RECT& rc) {
  const float scale = 96.0f / static_cast<float>(g_dpi);
  return D2D1::RectF(static_cast<float>(rc.left) * scale,
                     static_cast<float>(rc.top) * scale,
                     static_cast<float>(rc.right) * scale,
                     static_cast<float>(rc.bottom) * scale);
}

bool ensure_format() {
  if (!g_dwrite) {
    return false;
  }
  if (g_format) {
    return true;
  }
  const HRESULT hr = g_dwrite->CreateTextFormat(
      kUIFont, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
      DWRITE_FONT_STRETCH_NORMAL, font_size_dip(g_dpi), L"en-us", &g_format);
  if (FAILED(hr)) {
    return false;
  }
  g_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  return true;
}

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
  return ensure_format() && ensure_dc_rt();
}

void text_render_shutdown() {
  g_drawing = false;
  g_dc_rt.Reset();
  g_format.Reset();
  g_dwrite.Reset();
  g_d2d.Reset();
  g_dpi = 96;
}

void text_render_set_dpi(int dpi) {
  if (dpi <= 0) {
    dpi = 96;
  }
  if (g_dpi == dpi && g_format) {
    return;
  }
  g_dpi = dpi;
  g_format.Reset();
  g_dc_rt.Reset();
  ensure_format();
  ensure_dc_rt();
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
