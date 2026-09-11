module;

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string>

export module window.Lyrics;

import plugin.Config;

export class Lyrics {
private:
    // 无歌词数据（管道尚未推送）时显示的占位文本与占位色：
    // 颜色按当前主题取（深色 0xFF9E9E9E / 浅色 0xFF767676，见 Config.cppm
    // THEME_DARK/THEME_LIGHT.placeholder），避免浅色模式下浅灰占位不可读。
    static constexpr const wchar_t *PLACEHOLDER_TEXT = L"等待 go-musicfox…";

    ID2D1RenderTarget *render = nullptr;
    IDWriteFactory *dwrite = nullptr;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format1{};
    Microsoft::WRL::ComPtr<IDWriteTextFormat> format2{};
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout1{};
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout2{};
    DWRITE_TEXT_METRICS metrics1{};
    DWRITE_TEXT_METRICS metrics2{};

public:
    Lyrics(ID2D1RenderTarget *render, IDWriteFactory *dwrite) {
        this->render = render;
        this->dwrite = dwrite;
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

        const auto [width, height] = this->render->GetSize();

        this->dwrite->CreateTextFormat(
            config.font_family.data(),
            nullptr,
            config.weight_primary,
            config.slope_primary,
            DWRITE_FONT_STRETCH_NORMAL,
            config.size_primary,
            L"zh-CN",
            &format1
        );
        this->dwrite->CreateTextFormat(
            config.font_family.data(),
            nullptr,
            config.weight_secondary,
            config.slope_secondary,
            DWRITE_FONT_STRETCH_NORMAL,
            config.size_secondary,
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

        // 字号自适应：窗口高 = 任务栏高（约 32px），两行 14pt 文本行高约 37px，
        // 超出时 margin 为负 → rect 溢出被 clip 裁剪（翻译两行显示不全）。
        // 两行总高超出窗口高时按比例缩小字号（下限 0.6 倍保护可读性），重建
        // format/layout 后重新测高；无 secondary 时仅按单行判断，同样缩放。
        auto scale = 1.0f;
        const auto totalHeight = this->metrics1.height + this->metrics2.height;
        if (totalHeight > height) {
            scale = height / totalHeight;
            if (scale < 0.6f) {
                scale = 0.6f; // 最小字号保护，避免缩得太小不可读
            }
        }
        if (scale < 1.0f) {
            // 用缩放后的字号（float）重建 format（成员变量重建后，第二步
            // createText 绘制时自然生效），同参不同字号。
            const auto size1 = static_cast<float>(config.size_primary) * scale;
            const auto size2 = static_cast<float>(config.size_secondary) * scale;
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

        auto margin = (height - this->metrics1.height - this->metrics2.height) / 2;
        if (margin < 0.0f) {
            margin = 0.0f; // 防负：总高仍超窗口时从顶部绘制，避免负 margin 溢出被裁剪
        }
        const auto rect1 = D2D1::RectF(margin, margin, width - margin, margin + this->metrics1.height);
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
