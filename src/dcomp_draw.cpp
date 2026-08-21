#include "dcomp_draw.h"

#include <d2d1_1.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdio>

using Microsoft::WRL::ComPtr;

namespace ustb {
namespace {

HWND g_hwnd = nullptr;
int g_w = 0;
int g_h = 0;
int g_dpi = 96;

ComPtr<ID3D11Device> g_d3d;
ComPtr<IDXGIDevice> g_dxgi_device;
ComPtr<IDXGIFactory2> g_dxgi_factory;
ComPtr<IDXGISwapChain1> g_swap;
ComPtr<ID2D1Factory1> g_d2d_factory;
ComPtr<ID2D1Device> g_d2d_device;
ComPtr<ID2D1DeviceContext> g_d2d_dc;
ComPtr<IDWriteFactory> g_dwrite;
ComPtr<IDWriteTextFormat> g_format;
ComPtr<IDCompositionDevice> g_dcomp;
ComPtr<IDCompositionTarget> g_target;
ComPtr<IDCompositionVisual> g_visual;

void log_hr(const char* what, HRESULT hr) {
  FILE* f = nullptr;
  if (fopen_s(&f, "C:\\Users\\jason\\Desktop\\USTB-traffic\\build\\dcomp_dbg.txt",
              "a") == 0 &&
      f) {
    std::fprintf(f, "%s hr=0x%08lX\n", what, static_cast<unsigned long>(hr));
    std::fclose(f);
  }
}

void reset_swap() {
  if (g_d2d_dc) {
    g_d2d_dc->SetTarget(nullptr);
  }
  g_swap.Reset();
  g_w = 0;
  g_h = 0;
}

}  // namespace

bool dcomp_ready() { return g_dcomp && g_target && g_visual && g_d2d_dc; }

void dcomp_shutdown() {
  reset_swap();
  g_format.Reset();
  g_dwrite.Reset();
  g_visual.Reset();
  g_target.Reset();
  g_dcomp.Reset();
  g_d2d_dc.Reset();
  g_d2d_device.Reset();
  g_d2d_factory.Reset();
  g_dxgi_factory.Reset();
  g_dxgi_device.Reset();
  g_d3d.Reset();
  g_hwnd = nullptr;
}

bool dcomp_init(HWND hwnd) {
  dcomp_shutdown();
  g_hwnd = hwnd;
  if (!hwnd) {
    return false;
  }

  UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
  flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
  D3D_FEATURE_LEVEL fl{};
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 flags, nullptr, 0, D3D11_SDK_VERSION, &g_d3d,
                                 &fl, nullptr);
  if (FAILED(hr)) {
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                           D3D11_SDK_VERSION, &g_d3d, &fl, nullptr);
  }
  if (FAILED(hr)) {
    log_hr("D3D11CreateDevice", hr);
    dcomp_shutdown();
    return false;
  }

  hr = g_d3d.As(&g_dxgi_device);
  if (FAILED(hr)) {
    log_hr("Query IDXGIDevice", hr);
    dcomp_shutdown();
    return false;
  }

  hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                         IID_PPV_ARGS(&g_d2d_factory));
  if (FAILED(hr)) {
    log_hr("D2D1CreateFactory", hr);
    dcomp_shutdown();
    return false;
  }
  hr = g_d2d_factory->CreateDevice(g_dxgi_device.Get(), &g_d2d_device);
  if (FAILED(hr)) {
    log_hr("CreateDevice D2D", hr);
    dcomp_shutdown();
    return false;
  }
  hr = g_d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                         &g_d2d_dc);
  if (FAILED(hr)) {
    log_hr("CreateDeviceContext", hr);
    dcomp_shutdown();
    return false;
  }

  hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                           reinterpret_cast<IUnknown**>(g_dwrite.ReleaseAndGetAddressOf()));
  if (FAILED(hr)) {
    log_hr("DWriteCreateFactory", hr);
    dcomp_shutdown();
    return false;
  }

  ComPtr<IDXGIAdapter> adapter;
  hr = g_dxgi_device->GetAdapter(&adapter);
  if (FAILED(hr)) {
    log_hr("GetAdapter", hr);
    dcomp_shutdown();
    return false;
  }
  hr = adapter->GetParent(IID_PPV_ARGS(&g_dxgi_factory));
  if (FAILED(hr)) {
    log_hr("GetParent DXGI factory", hr);
    dcomp_shutdown();
    return false;
  }

  hr = DCompositionCreateDevice(g_dxgi_device.Get(), IID_PPV_ARGS(&g_dcomp));
  if (FAILED(hr)) {
    log_hr("DCompositionCreateDevice", hr);
    dcomp_shutdown();
    return false;
  }
  hr = g_dcomp->CreateTargetForHwnd(hwnd, TRUE, &g_target);
  if (FAILED(hr)) {
    log_hr("CreateTargetForHwnd", hr);
    dcomp_shutdown();
    return false;
  }
  hr = g_dcomp->CreateVisual(&g_visual);
  if (FAILED(hr)) {
    log_hr("CreateVisual", hr);
    dcomp_shutdown();
    return false;
  }
  hr = g_target->SetRoot(g_visual.Get());
  if (FAILED(hr)) {
    log_hr("SetRoot", hr);
    dcomp_shutdown();
    return false;
  }
  g_dcomp->Commit();
  return true;
}

bool dcomp_resize(int width, int height, int dpi) {
  if (!dcomp_ready() || width <= 0 || height <= 0) {
    return false;
  }
  if (dpi <= 0) {
    dpi = 96;
  }
  if (g_swap && g_w == width && g_h == height && g_dpi == dpi) {
    return true;
  }

  reset_swap();
  g_dpi = dpi;
  g_d2d_dc->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));

  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = static_cast<UINT>(width);
  desc.Height = static_cast<UINT>(height);
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
  desc.Scaling = DXGI_SCALING_STRETCH;

  HRESULT hr = g_dxgi_factory->CreateSwapChainForComposition(
      g_d3d.Get(), &desc, nullptr, &g_swap);
  if (FAILED(hr)) {
    log_hr("CreateSwapChainForComposition", hr);
    return false;
  }
  hr = g_visual->SetContent(g_swap.Get());
  if (FAILED(hr)) {
    log_hr("SetContent", hr);
    return false;
  }

  ComPtr<IDXGISurface> surface;
  hr = g_swap->GetBuffer(0, IID_PPV_ARGS(&surface));
  if (FAILED(hr)) {
    log_hr("GetBuffer", hr);
    return false;
  }

  const float fdpi = static_cast<float>(dpi);
  D2D1_BITMAP_PROPERTIES1 bp{};
  bp.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
  bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
  bp.dpiX = fdpi;
  bp.dpiY = fdpi;
  bp.bitmapOptions =
      D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;

  ComPtr<ID2D1Bitmap1> bitmap;
  hr = g_d2d_dc->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &bitmap);
  if (FAILED(hr)) {
    log_hr("CreateBitmapFromDxgiSurface", hr);
    return false;
  }
  g_d2d_dc->SetTarget(bitmap.Get());
  g_w = width;
  g_h = height;

  g_format.Reset();
  const float dip_size = 12.0f;  // 9pt
  hr = g_dwrite->CreateTextFormat(
      L"Microsoft YaHei UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, dip_size, L"zh-CN",
      &g_format);
  if (FAILED(hr)) {
    log_hr("CreateTextFormat", hr);
    return false;
  }
  g_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
  g_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  g_dcomp->Commit();
  return true;
}

void dcomp_draw(const wchar_t* line1, const wchar_t* line2, COLORREF fg) {
  if (!dcomp_ready() || !g_swap || !g_format || g_w <= 0 || g_h <= 0) {
    return;
  }

  g_d2d_dc->BeginDraw();
  g_d2d_dc->Clear(D2D1::ColorF(0, 0, 0, 0));

  ComPtr<ID2D1SolidColorBrush> brush;
  const float r = GetRValue(fg) / 255.0f;
  const float g = GetGValue(fg) / 255.0f;
  const float b = GetBValue(fg) / 255.0f;
  HRESULT hr = g_d2d_dc->CreateSolidColorBrush(D2D1::ColorF(r, g, b, 1.0f),
                                               &brush);
  if (FAILED(hr)) {
    g_d2d_dc->EndDraw();
    return;
  }

  const float dpi_scale = 96.0f / static_cast<float>(g_dpi);
  const float w = static_cast<float>(g_w) * dpi_scale;
  const float h = static_cast<float>(g_h) * dpi_scale;
  const float pad = 2.0f * static_cast<float>(g_dpi) / 96.0f * dpi_scale;
  D2D1_RECT_F top{pad, 0, w, h * 0.5f};
  D2D1_RECT_F bottom{pad, h * 0.5f, w, h};

  if (line1) {
    g_d2d_dc->DrawTextW(line1, static_cast<UINT32>(wcslen(line1)), g_format.Get(),
                        top, brush.Get());
  }
  if (line2) {
    g_d2d_dc->DrawTextW(line2, static_cast<UINT32>(wcslen(line2)), g_format.Get(),
                        bottom, brush.Get());
  }

  hr = g_d2d_dc->EndDraw();
  if (FAILED(hr)) {
    log_hr("EndDraw", hr);
    return;
  }
  g_swap->Present(1, 0);
  g_dcomp->Commit();
}

}  // namespace ustb
