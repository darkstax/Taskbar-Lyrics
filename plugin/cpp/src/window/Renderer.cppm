module;

#include <Windows.h> // GetDpiForWindow / UpdateLayeredWindow / CreateDIBSection（user32+gdi32，项目已链接）
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string>

export module window.Renderer;

import window.Lyrics;
import plugin.Config;
import util.Log;

// 渲染器：离屏 32 位预乘 alpha 表面 + UpdateLayeredWindow（ULW_ALPHA）逐像素透明。
//
// 为什么不用 LWA_COLORKEY（历史方案，已废弃）：
//   颜色键透明要求"底色与键色逐字节相同"，且"字色禁止精确等于键色"（否则被挖空），
//   字形抗锯齿边缘只能向键色渐变——键色与真实任务栏不一致时就会留下光晕/灰边；
//   键色还要随主题在黑白之间切，主题判错就会让字芯压在深色背景上不可见
//   （用户实测的"歌词发灰看不清"根因）。这是"内色（字芯）与外色（边缘）都被键色绑架"。
//   逐像素 alpha 下：窗口背景 alpha=0 是真透明，字形边缘自由混向透明，
//   由 DWM 与真实任务栏合成 —— 内色就是配置字色，外色自动等于真实背景，
//   键色/挖空保护/精度对齐这一整类问题从根上消失。
//
// 与历史的 DComp 冻结结论的关系：旧实现（5ca6c32/812eaf5）是 DComp+DXGI 交换链，
// 且窗口是 Shell_TrayWnd 的 WS_CHILD 子窗口 —— 任务栏右键菜单模态会暂停 explorer
// 自身的合成，歌词随之冻结；0b5d7a7 因此改回 COLORKEY。现在窗口是独立顶层窗口
// （非子窗口），ULW 由 win32k 直排到本窗口、不依赖 WM_PAINT，前提已不同。
//
// 纪律：
//   ① 同一 WS_EX_LAYERED 窗口上 SetLayeredWindowAttributes 与 ULW 互斥——后者被调用
//      会夺回合成权、丢弃逐像素 alpha。全项目禁止再调 SetLayeredWindowAttributes。
//   ② 所有 D2D/ULW 调用仅在主线程（与窗口消息同线程，见 Window::runner）。
//   ③ 位置/尺寸变化后必须重新 present：ULW 的 pptDst/psize 就是它与窗口的对齐依据。
export class Renderer {
private:
    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory{};
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> dcRenderTarget{};
    Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory{};

    HWND hwnd = nullptr;
    HDC memDc = nullptr;       // 与屏幕兼容的内存 DC，DIB 选入其中
    HBITMAP dib = nullptr;     // top-down 32bpp DIB = ULW 的源表面
    HGDIOBJ oldDib = nullptr;  // 重建时换回，避免句柄泄漏
    void *dibBits = nullptr;

    // 表面元数据：物理像素 + 窗口 DPI（唯一的"像素↔DIP"真相源，供 Lyrics 使用）
    UINT surfaceW = 0;
    UINT surfaceH = 0;
    UINT surfaceDpi = 96;

    // 解锁态（仅垂直任务栏）窗口底衬不透明度：0=锁定（背景全透明），210=解锁半透明底。
    // 注意与旧 LWA_ALPHA 的差别：这里只让"底衬"半透明，文字保持全不透明（边缘锐利）。
    unsigned char overlayAlpha = 0;

    bool presenting = false; // ULW 重入保护（ULW 可能同步触发 WM_SIZE 等消息）

    auto releaseSurface() -> void {
        if (this->dib != nullptr) {
            if (this->memDc != nullptr && this->oldDib != nullptr) {
                SelectObject(this->memDc, this->oldDib);
            }
            DeleteObject(this->dib);
            this->dib = nullptr;
            this->oldDib = nullptr;
            this->dibBits = nullptr;
        }
        this->surfaceW = 0;
        this->surfaceH = 0;
    }

    // 表面尺寸/DPI 变化的唯一重建入口（幂等；换 DIB 后必须重新 BindDC）
    auto ensureSurface(const UINT width, const UINT height, const UINT dpi) -> bool {
        if (width == 0 || height == 0) {
            this->releaseSurface();
            return false;
        }
        if (this->dib != nullptr && width == this->surfaceW && height == this->surfaceH && dpi == this->surfaceDpi) {
            return true;
        }
        if (this->memDc == nullptr || !this->dcRenderTarget) {
            return false;
        }
        this->releaseSurface();

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = static_cast<LONG>(width);
        bmi.bmiHeader.biHeight = -static_cast<LONG>(height); // 负值 = top-down，与 D2D 绘制坐标一致
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB; // 内存即 BGRA，匹配 DXGI_FORMAT_B8G8R8A8_UNORM
        void *bits = nullptr;
        const auto newDib = CreateDIBSection(this->memDc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (newDib == nullptr || bits == nullptr) {
            if (newDib != nullptr) {
                DeleteObject(newDib);
            }
            Log::event(L"创建离屏 DIB 失败 (error " + std::to_wstring(GetLastError()) + L")");
            return false;
        }
        this->oldDib = SelectObject(this->memDc, newDib);
        this->dib = newDib;
        this->dibBits = bits;
        this->surfaceW = width;
        this->surfaceH = height;
        this->surfaceDpi = dpi;

        // DCRenderTarget 的尺寸来自 BindDC 的矩形（像素），DPI 决定 DIP 映射
        this->dcRenderTarget->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
        const RECT rc{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
        this->dcRenderTarget->BindDC(this->memDc, &rc);
        Log::event(
            L"离屏表面重建: " + std::to_wstring(width) + L"x" + std::to_wstring(height) +
            L" @" + std::to_wstring(dpi) + L"dpi"
        );
        return true;
    }

    // 渲染一帧到离屏表面（背景恒全透明；解锁态先铺半透明底衬，再画文字）
    auto renderFrame() -> void {
        const auto target = this->dcRenderTarget.Get();
        const auto dpi = static_cast<float>(this->surfaceDpi == 0 ? 96 : this->surfaceDpi);
        const auto pxToDip = 96.0f / dpi;
        target->BeginDraw();
        target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f)); // alpha=0：真透明底
        if (this->overlayAlpha != 0) {
            // 解锁态底衬：颜色随主题（深色黑/浅色白）以便与字色形成对比；
            // 只有底衬带 alpha，文字仍全不透明（旧 LWA_ALPHA 会把字一起变淡）
            Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> panel{};
            const auto a = static_cast<float>(this->overlayAlpha) / 255.0f;
            target->CreateSolidColorBrush(
                config.color_theme_light ? D2D1::ColorF(1.0f, 1.0f, 1.0f, a) : D2D1::ColorF(0.0f, 0.0f, 0.0f, a),
                &panel
            );
            target->FillRectangle(
                D2D1::RectF(
                    0.0f, 0.0f,
                    static_cast<float>(this->surfaceW) * pxToDip,
                    static_cast<float>(this->surfaceH) * pxToDip
                ),
                panel.Get()
            );
        }
        // 灰度抗锯齿：真 alpha 下边缘向透明过渡，与任意任务栏背景自然合成（无键色可撞）
        Lyrics lyrics{
            target,
            this->dwriteFactory.Get(),
            RenderMetrics{this->surfaceW, this->surfaceH, this->surfaceDpi}
        };
        lyrics.onDraw();
        target->EndDraw();
    }

public:
    // 持有裸 HDC/HBITMAP，禁止拷贝（否则析构双重释放）
    Renderer() = default;
    Renderer(const Renderer &) = delete;
    auto operator=(const Renderer &) -> Renderer & = delete;

    // 切换解锁态底衬（仅改 alpha 参数，下一次 present 生效；不碰窗口分层属性）
    auto setOverlayAlpha(const unsigned char alpha) -> void {
        this->overlayAlpha = alpha;
    }

    auto onCreate(const HWND target) -> void {
        this->hwnd = target;
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&this->d2dFactory));
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(this->dwriteFactory), &this->dwriteFactory);
        this->memDc = CreateCompatibleDC(nullptr);
        if (this->d2dFactory && this->memDc != nullptr) {
            // 预乘 alpha 的 BGRA 目标：D2D 输出即可直接交给 ULW（AC_SRC_ALPHA）
            const auto props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                96.0f,
                96.0f
            );
            this->d2dFactory->CreateDCRenderTarget(&props, &this->dcRenderTarget);
        }
        if (this->dcRenderTarget) {
            this->dcRenderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        } else {
            Log::event(L"创建 DCRenderTarget 失败 (error " + std::to_wstring(GetLastError()) + L")");
        }
        Log::event(L"渲染路径：离屏预乘 alpha + UpdateLayeredWindow");
    }

    auto onSize(const UINT width, const UINT height, const UINT dpi) -> void {
        this->ensureSurface(width, height, dpi);
    }

    // 渲染并提交（present）。语义与旧的"直接 onPaint 绕过 WM_PAINT 抑制"一致，
    // 但不再依赖 WM_PAINT：ULW 由 win32k 直接把位图排进窗口层。
    // 位置/尺寸变化后必须重新调用本方法（ULW 的 pptDst/psize 与窗口对齐）。
    auto onPaint() -> void {
        if (this->hwnd == nullptr || !this->dcRenderTarget) {
            return;
        }
        if (this->presenting) {
            return; // 重入（ULW 可能同步派发消息）
        }
        if (!IsWindowVisible(this->hwnd)) {
            return; // 全屏隐藏期间不提交；恢复可见后由 Window 主动重新 present
        }
        RECT rc{};
        if (!GetWindowRect(this->hwnd, &rc)) {
            return;
        }
        const auto width = static_cast<UINT>(rc.right - rc.left);
        const auto height = static_cast<UINT>(rc.bottom - rc.top);
        if (!this->ensureSurface(width, height, GetDpiForWindow(this->hwnd))) {
            return;
        }

        this->presenting = true;
        this->renderFrame();

        POINT dst{rc.left, rc.top}; // 物理屏幕坐标（本进程 PerMonitorV2）
        SIZE size{static_cast<LONG>(width), static_cast<LONG>(height)}; // 必须与 DIB 尺寸一致
        POINT src{0, 0};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA}; // UpdateLayeredWindow 需要非 const
        if (!UpdateLayeredWindow(this->hwnd, nullptr, &dst, &size, this->memDc, &src, 0, &blend, ULW_ALPHA)) {
            Log::event(L"UpdateLayeredWindow 失败 (error " + std::to_wstring(GetLastError()) + L")");
        }
        this->presenting = false;
    }

    ~Renderer() {
        this->releaseSurface();
        if (this->memDc != nullptr) {
            DeleteDC(this->memDc);
            this->memDc = nullptr;
        }
    }
};
