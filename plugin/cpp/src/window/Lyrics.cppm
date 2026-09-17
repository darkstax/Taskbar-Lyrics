module;

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string>

export module window.Lyrics;

import plugin.Config;

// 渲染目标元数据（由 Renderer 显式传入，取代旧代码对 RT 的 GetSize()/GetDpi() 查询）：
// - pxW/pxH 是**物理像素**（离屏 DIB 尺寸 = 窗口尺寸），dpi 是窗口 DPI；
// - 显式传入的原因：ID2D1RenderTarget::GetSize() 的语义随 RT 类型而异（文档规定返回 DIP，
//   而旧代码把它当物理像素再乘一次 96/dpi，等于做了两次换算，非 96DPI 下文字盒只有
//   实际宽度的 (96/dpi)² —— 本次修正为单次换算，并把这个边界收敛到 Renderer 一处。
export struct RenderMetrics {
    unsigned int pxW = 0;
    unsigned int pxH = 0;
    unsigned int dpi = 96;
};

export class Lyrics {
private:
    // 无歌词数据（管道尚未推送）时显示的占位文本与占位色：
    // 颜色按当前主题取（深色 0xFF9E9E9E / 浅色 0xFF767676，见 Config.cppm
    // THEME_DARK/THEME_LIGHT.placeholder），避免浅色模式下浅灰占位不可读。
    static constexpr const wchar_t *PLACEHOLDER_TEXT = L"等待 go-musicfox…";

    ID2D1RenderTarget *render = nullptr;
    IDWriteFactory *dwrite = nullptr;
    RenderMetrics metrics{};
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format1{};
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format2{};
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout1{};
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout2{};
    DWRITE_TEXT_METRICS metrics1{};
    DWRITE_TEXT_METRICS metrics2{};

public:
    Lyrics(ID2D1RenderTarget *render, IDWriteFactory *dwrite, const RenderMetrics &metrics) {
        this->render = render;
        this->dwrite = dwrite;
        this->metrics = metrics;
    }

    auto onDraw() -> void {
        this->format1.Reset();
        this->format2.Reset();
        this->layout1.Reset();
        this->layout2.Reset();

        // 无歌词数据（管道尚未推送）时绘制占位文本（颜色随主题），避免空白窗口；
        // 占位与歌词共用同一 TextLayout 创建/绘制路径。
        // 字色读 *_active（每帧由主题/显式覆盖解析，主线程维护）。
        const bool hasLyric = !config.lyric_primary.empty();
        const auto primaryText = hasLyric ? config.lyric_primary : Lyrics::PLACEHOLDER_TEXT;
        const auto placeholderColor = config.color_theme_light ? THEME_LIGHT.placeholder : THEME_DARK.placeholder;
        const auto primaryColor = hasLyric ? config.color_primary_active : placeholderColor;
        const bool hasSecondary = hasLyric && !config.lyric_secondary.empty();

        // 尺寸来源：Renderer 显式传入的物理像素 + 窗口 DPI（不再查 GetSize()/GetDpi()）。
        // DWrite/D2D 绘制坐标是 DIP（= 物理像素 × 96/dpi），故这里做**单次**换算。
        // （历史缺陷：旧代码把 GetSize() 的 DIP 当物理像素又乘一次 96/dpi，非 96DPI 下
        //   文字盒只有实际尺寸的 (96/dpi)²，字号自适应因此系统性偏小；已修正。）
        const auto dpiX = static_cast<float>(this->metrics.dpi > 0 ? this->metrics.dpi : 96);
        const auto dpiY = dpiX;
        const auto width = static_cast<float>(this->metrics.pxW) * 96.f / dpiX;
        const auto height = static_cast<float>(this->metrics.pxH) * 96.f / dpiY;
        // 字号语义保持“物理像素”（与旧版一致，用户预期不变）：size=14 即 14 物理像素行高。
        // 这里把 size_* 按 96/dpi 折算成 DIP，物理大小保持不变，清晰度享受原生栅格化。
        const auto pxToDip = 96.f / dpiY;
        const auto dipSizePrimary = static_cast<float>(config.size_primary) * pxToDip;
        const auto dipSizeSecondary = static_cast<float>(config.size_secondary) * pxToDip;

        this->dwrite->CreateTextFormat(
            config.font_family.data(),
            nullptr,
            config.weight_primary,
            config.slope_primary,
            DWRITE_FONT_STRETCH_NORMAL,
            dipSizePrimary,
            L"zh-CN",
            &format1
        );
        this->dwrite->CreateTextFormat(
            config.font_family.data(),
            nullptr,
            config.weight_secondary,
            config.slope_secondary,
            DWRITE_FONT_STRETCH_NORMAL,
            dipSizeSecondary,
            L"zh-CN",
            &format2
        );

        // 第一步：以窗口尺寸创建布局，测量文本高度以计算绘制区域。
        // 行高说明：NO_WRAP 只禁止自动换行，显式 \n（go-musicfox 桌面歌词风格
        // 的拼接换行）仍产生多行；DWRITE_TEXT_METRICS.height 为全部行的总高度，
        // 多行场景直接用它作总高即可。
        this->dwrite->CreateTextLayout(primaryText.data(), static_cast<UINT32>(primaryText.size()), this->format1.Get(), width, height, &this->layout1);
        this->layout1->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        this->layout1->GetMetrics(&this->metrics1);
        if (hasSecondary) {
            this->dwrite->CreateTextLayout(config.lyric_secondary.data(), static_cast<UINT32>(config.lyric_secondary.size()), this->format2.Get(), width, height, &this->layout2);
            this->layout2->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            this->layout2->GetMetrics(&this->metrics2);
        } else {
            this->metrics2 = {};
        }

        // 字号自适应（按任务栏高贴合）：窗口高 = 任务栏物理高（applyLayout 直接
        // 取 Appbar 完整矩形），故此处直接以窗口高为目标：
        // - 带翻译（两行）：两行合计占满 height；
        // - 不带翻译（单行）：占满整个 height。
        // 双向贴合：不足则放大、超出则缩小（0.6 下限保护极端、4.0 上限保护异常窗口），
        // 主/副字号比例保持 config 设定不变（注意：双向贴合下 size_* 只决定两行比例，
        // 绝对大小由本系数决定）。
        // 目标总高 = 1.0 × height：恰好贴合、不裁切。
        // 历史：系数曾是 1.18（意图"em≈0.9×栏高/行"），但那是建立在 GetSize 双次换算
        // 得到的偏小高度上的；修正为单次换算后 1.18 会让上下各溢出 ~9%，实测拉丁 g/j
        // 下伸部与 CJK 顶部被切，故回到 1.0。
        auto scale = 1.0f;
        const auto totalHeight = this->metrics1.height + this->metrics2.height;
        if (totalHeight > 0.0f) {
            scale = height / totalHeight;
            if (scale < 0.6f) {
                scale = 0.6f; // 最小字号保护，避免缩得太小不可读
            }
            if (scale > 4.0f) {
                scale = 4.0f; // 异常超高窗口保护（如垂直模式记忆位置贴大屏）
            }
        }
        if (scale != 1.0f) {
            // 用缩放后的字号（float）重建 format（成员变量重建后，第二步
            // createText 绘制时自然生效），同参不同字号。
            const auto size1 = dipSizePrimary * scale;
            const auto size2 = dipSizeSecondary * scale;
            this->format1.Reset();
            this->dwrite->CreateTextFormat(
                config.font_family.data(),
                nullptr,
                config.weight_primary,
                config.slope_primary,
                DWRITE_FONT_STRETCH_NORMAL,
                size1,
                L"zh-CN",
                &this->format1
            );
            if (hasSecondary) {
                this->format2.Reset();
                this->dwrite->CreateTextFormat(
                    config.font_family.data(),
                    nullptr,
                    config.weight_secondary,
                    config.slope_secondary,
                    DWRITE_FONT_STRETCH_NORMAL,
                    size2,
                    L"zh-CN",
                    &this->format2
                );
            }
            // 重建布局并重新测高（缩放后两行总高应 ≤ 窗口高）
            this->layout1.Reset();
            this->dwrite->CreateTextLayout(primaryText.data(), static_cast<UINT32>(primaryText.size()), this->format1.Get(), width, height, &this->layout1);
            this->layout1->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            this->layout1->GetMetrics(&this->metrics1);
            if (hasSecondary) {
                this->layout2.Reset();
                this->dwrite->CreateTextLayout(config.lyric_secondary.data(), static_cast<UINT32>(config.lyric_secondary.size()), this->format2.Get(), width, height, &this->layout2);
                this->layout2->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                this->layout2->GetMetrics(&this->metrics2);
            }
        }

        // 允许负 margin：贴合放大后文字块略高于窗口，对称居中溢出、由边缘裁切
        //（旧 clamp≥0 会把第二行整体推出窗口底，只裁底部不裁顶部，视觉重心偏上）。
        // 边缘留白微调（用户要求）：文字整体右移 4px、下移 1px（物理像素，
        // 按 dpi 折算 DIP）——负 margin 下首字会直接撞窗口左缘、顶部笔画贴边，
        // 留出呼吸感观感更好。右缘同步平移，保持行宽不被提前裁切。
        const auto padX = 4.f * 96.f / dpiX;
        const auto padY = 1.f * 96.f / dpiY;
        auto margin = (height - this->metrics1.height - this->metrics2.height) / 2;
        const auto rect1 = D2D1::RectF(margin + padX, margin + padY, width - margin + padX, margin + this->metrics1.height + padY);
        D2D1_RECT_F rect2{};
        if (hasSecondary) {
            rect2 = D2D1::RectF(rect1.left, rect1.bottom, rect1.right, rect1.bottom + this->metrics2.height);
        }

        // 第二步：按实际绘制区域重建布局（应用对齐/下划线/删除线）并绘制。
        // 居中模式（window_alignment=2）下窗口横跨整个任务栏，文字强制水平居中
        // （用户直觉的"歌词在任务栏中间"），其余模式遵循 align_primary/secondary。
        const auto centerMode = (config.window_alignment == TASKBAR_WINDOW_ALIGNMENT::TASKBAR_WINDOW_ALIGNMENT_CENTER);
        const auto align1 = centerMode ? DWRITE_TEXT_ALIGNMENT::DWRITE_TEXT_ALIGNMENT_CENTER : config.align_primary;
        this->createText(rect1, this->format1.Get(), this->layout1, primaryText, align1, config.underline_primary, config.strikethrough_primary);
        this->drawText(rect1, this->layout1.Get(), primaryColor);
        if (hasSecondary) {
            const auto align2 = centerMode ? DWRITE_TEXT_ALIGNMENT::DWRITE_TEXT_ALIGNMENT_CENTER : config.align_secondary;
            this->createText(rect2, this->format2.Get(), this->layout2, config.lyric_secondary, align2, config.underline_secondary, config.strikethrough_secondary);
            this->drawText(rect2, this->layout2.Get(), config.color_secondary_active);
        }
    }

    auto createText(
        const D2D1_RECT_F &rect,
        IDWriteTextFormat *format,
        Microsoft::WRL::ComPtr<IDWriteTextLayout> &layout,
        const std::wstring &string,
        const DWRITE_TEXT_ALIGNMENT alignment,
        const bool underline,
        const bool strikethrough
    ) -> void {
        // 按实际绘制区域重建布局（原实现把新布局赋给局部参数导致丢失：
        // 对齐/下划线/删除线设置不生效且每次绘制泄漏一个 layout，此处修复）
        layout.Reset();
        this->dwrite->CreateTextLayout(string.data(), static_cast<UINT32>(string.size()), format, rect.right - rect.left, rect.bottom - rect.top, &layout);
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        layout->SetTextAlignment(alignment);
        layout->SetUnderline(underline, DWRITE_TEXT_RANGE(0, static_cast<UINT32>(string.size())));
        layout->SetStrikethrough(strikethrough, DWRITE_TEXT_RANGE(0, static_cast<UINT32>(string.size())));
    }

    auto drawText(const D2D1_RECT_F &rect, IDWriteTextLayout *layout, const unsigned int color) -> void {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush{};
        this->render->CreateSolidColorBrush(D2D1::ColorF(color, (color >> 24 & 0xFF) / 255.f), &brush);
        this->render->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_ALIASED);
        this->render->DrawTextLayout(
            D2D1::Point2F(rect.left, rect.top),
            layout,
            brush.Get(),
            D2D1_DRAW_TEXT_OPTIONS_NO_SNAP | D2D1_DRAW_TEXT_OPTIONS_DISABLE_COLOR_BITMAP_SNAPPING | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT
        );
        this->render->PopAxisAlignedClip();
    }
};
