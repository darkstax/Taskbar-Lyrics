module;

#include <Windows.h> // GetDpiForWindow（user32，项目已链接）
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
//    光晕），最终用键色 + 灰度抗锯齿（文字边缘向底色渐变，底色=键色=透明，
//    深色任务栏上近乎不可见）。键色随主题：深色=黑、浅色=白——浅色任务栏上
//    黑底深字的边缘光晕可见（用户反馈“模糊/白边”），换白键色后边缘向白渐变
//    与浅色背景同色（详见 onPaint 注释）。
export class Renderer {
private:
    Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory{};
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> d2dRenderTarget{};
    Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory{};
    // 解锁态标记（LWA_ALPHA 路径，仅垂直任务栏解锁时为 true）：此路径无
    // COLORKEY，窗口整体半透明（alpha 210）。底色取色只随主题（见 onPaint），
    // 本标记仅记录当前分层路径供诊断/后续扩展，不参与取色判定。
    [[maybe_unused]] bool alphaMode = false;

public:
    // 切换解锁/锁定渲染路径（主线程，与 SetLayeredWindowAttributes 调用同步）
    auto setAlphaMode(const bool alpha) -> void {
        this->alphaMode = alpha;
    }

    // 当前主题下会被 COLORKEY 挖空的精确色（测试/诊断用，与 Config::themeKeyRgb 一致）
    auto keyColorArgb() const -> unsigned int {
        return config.color_theme_light ? 0x00FFFFFFu : 0x00000000u;
    }

    auto onCreate(const HWND hwnd) -> void {
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&this->d2dFactory));
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(this->dwriteFactory), &this->dwriteFactory);
        auto props = D2D1::RenderTargetProperties();
        const auto hwndProps = D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(1, 1));
        this->d2dFactory->CreateHwndRenderTarget(props, hwndProps, &this->d2dRenderTarget);
        // 灰度抗锯齿：键色透明下文字边缘为灰阶而非彩边（ClearType 在键色背景上会产生色边）
        this->d2dRenderTarget->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        // DPI 修正（用户反馈“字特别模糊、横向拖影、上有白边”的真因）：
        // HwndRenderTarget 默认 dpiX=dpiY=96，而本进程是 PerMonitorV2 感知
        // （app.manifest），125% 缩放下窗口物理像素=逻辑像素×1.25，D2D 仍按
        // 96DPI 把内容画小后由 DWM 位图拉伸上屏 → 整体横向模糊。把渲染目标
        // DPI 设为窗口真实 DPI，使 1 DIP=1 物理像素×(96/dpi) 换算正确、文字
        // 按原生分辨率栅格化（字号视觉大小不变，清晰度恢复）。
        const auto dpi = GetDpiForWindow(hwnd);
        if (dpi != 0) {
            this->d2dRenderTarget->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
        }
        // 分层属性（LWA_COLORKEY）由调用方 Window::applyLayeredMode 成对设置
        //（审查 G3：SetLayeredWindowAttributes 与 setAlphaMode 必须同调，不散写；
        // 本方法只建渲染目标，不碰窗口分层样式）。
    }

    auto onSize(const UINT width, const UINT height, const UINT dpi) -> void {
        if (this->d2dRenderTarget) {
            // 跨屏/改缩放时同步渲染目标 DPI（与 onCreate 同一修正，WM_DPICHANGED 后生效）
            if (dpi != 0) {
                this->d2dRenderTarget->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
            }
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
        // 底色必须与当前键色一致（LWA_COLORKEY 精确匹配才能整块变透明）：
        // 深色主题=黑（现状），浅色主题=白。
        // 为什么浅色要换白键色（用户反馈“字模糊、上有白边”）：灰阶抗锯齿下
        // 文字边缘是从字色向底色渐变的像素。黑底时白字边缘变灰，叠在深色任务
        // 栏上几乎不可见（锐利）；但浅色任务栏上深字边缘会向黑渐变，比字芯还深，
        // 形成一圈深色光晕（看起来模糊，字芯相对偏亮即“白边”）。换成白键色后
        // 边缘向白渐变，与浅色背景同色，光晕消失。
        // 解锁态（LWA_ALPHA，无键色）同样随主题取底色，避免浅色下深底深字糊成一团。
        const auto light = config.color_theme_light;
        this->d2dRenderTarget->Clear(light
                                        ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f)
                                        : D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f));
        lyrics.onDraw();
        this->d2dRenderTarget->EndDraw();
    }
};
