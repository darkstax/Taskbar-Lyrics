module;

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string>

export module window.Renderer;

import window.Lyrics;
import plugin.Config;
import util.Log;

// 渲染器：GDI 窗口路径（D2D HwndRenderTarget）+ 颜色键透明（LWA_COLORKEY）。
//
// 关键结论（菜单模态冻结排查，2026-08-04）：
// ① 任务栏右键菜单模态会抑制 WM_PAINT 派发（RedrawWindow/InvalidateRect
//    失效，实测菜单期间 onPaint 停止、关闭后恢复）——歌词更新路径
//    （WM_APP+1）直接调用 onPaint() 绕过该机制（最终修复）。
// ② DComp 合成链与 UpdateLayeredWindow 在菜单模态/任务栏区域均不可靠
//    （DComp 冻结；ULW 提交成功但不上屏），HwndRenderTarget 直接画窗口
//    客户区最可靠（窗口为独立顶层窗口，非任务栏子窗口）。
// ③ HwndRenderTarget 不支持 alpha：先试洋红键色（ClearType 彩边残留粉色
//    光晕），最终用黑色键色 + 灰度抗锯齿（文字边缘灰阶，深色任务栏上
//    近乎不可见）。
export class Renderer {
private:
    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory{};
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> d2dRenderTarget{};
    Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory{};
    // 解锁态标记（LWA_ALPHA 路径，仅垂直任务栏解锁时为 true）：
    // 此路径无 COLORKEY，底色可为任意值且整体半透明（alpha 210），
    // 底色随主题取色使“浅色模式解锁”不再深底深字糊成一团；
    // 锁定态（COLORKEY）底色永远保持精确 RGB(0,0,0)，不受此标记影响。
    bool alphaMode = false;

public:
    // 切换解锁/锁定渲染路径（主线程，与 SetLayeredWindowAttributes 调用同步）
    auto setAlphaMode(const bool alpha) -> void {
        this->alphaMode = alpha;
    }

    auto onCreate(const HWND hwnd) -> void {
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&this->d2dFactory));
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(this->dwriteFactory), &this->dwriteFactory);
        auto props = D2D1::RenderTargetProperties();
        const auto hwndProps = D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(1, 1));
        this->d2dFactory->CreateHwndRenderTarget(props, hwndProps, &this->d2dRenderTarget);
        // 灰度抗锯齿：键色透明下文字边缘为灰阶而非彩边（ClearType 在键色背景上会产生色边）
        this->d2dRenderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        // 分层属性（LWA_COLORKEY）由调用方 Window::applyLayeredMode 成对设置
        //（审查 G3：SetLayeredWindowAttributes 与 setAlphaMode 必须同调，不散写；
        // 本方法只建渲染目标，不碰窗口分层样式）。
    }

    auto onSize(const UINT width, const UINT height, const UINT dpi) -> void {
        if (this->d2dRenderTarget) {
            this->d2dRenderTarget->Resize(D2D1::SizeU(width, height));
        }
    }

    // 直接绘制（不经 WM_PAINT；WM_APP+1 歌词更新与 WM_PAINT 均调用本方法，
    // 菜单模态下 WM_PAINT 被抑制时仍能更新）
    auto onPaint() -> void {
        if (!this->d2dRenderTarget) {
            return;
        }
        Lyrics lyrics{
            this->d2dRenderTarget.Get(),
            this->dwriteFactory.Get()
        };
        this->d2dRenderTarget->BeginDraw();
        // 键色背景（黑色 → LWA_COLORKEY 透明）；仅解锁态（LWA_ALPHA，无键色）
        // 底色随主题：深色保持现状黑底，浅色改白底（搭配深色文字保持可读）。
        // 锁定分支的 Clear 色与键色一律不动（纯黑像素会被 COLORKEY 挖空）。
        const auto lightUnlocked = this->alphaMode && config.color_theme_light;
        this->d2dRenderTarget->Clear(lightUnlocked
                                        ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f)
                                        : D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f));
        lyrics.onDraw();
        this->d2dRenderTarget->EndDraw();
    }
};
