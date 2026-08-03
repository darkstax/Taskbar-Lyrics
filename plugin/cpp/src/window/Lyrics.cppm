module;

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string>

export module window.Lyrics;

import plugin.Config;

export class Lyrics {
private:
    // 无歌词数据（管道尚未推送）时显示的占位文本与浅灰颜色
    static constexpr const wchar_t *PLACEHOLDER_TEXT = L"等待 go-musicfox…";
    static constexpr unsigned int PLACEHOLDER_COLOR = 0xFF9E9E9E;

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

        // 无歌词数据（管道尚未推送）时绘制浅灰占位文本，避免空白窗口；
        // 占位与歌词共用同一 TextLayout 创建/绘制路径。
        const bool hasLyric = !config.lyric_primary.empty();
        const auto primaryText = hasLyric ? config.lyric_primary : Lyrics::PLACEHOLDER_TEXT;
        const auto primaryColor = hasLyric ? config.color_primary : Lyrics::PLACEHOLDER_COLOR;
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

        // 第一步：以窗口尺寸创建布局，测量文本高度以计算绘制区域
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

        const auto margin = (height - this->metrics1.height - this->metrics2.height) / 2;
        const auto rect1 = D2D1::RectF(margin, margin, width - margin, margin + this->metrics1.height);
        D2D1_RECT_F rect2{};
        if (hasSecondary) {
            rect2 = D2D1::RectF(rect1.left, rect1.bottom, rect1.right, rect1.bottom + this->metrics2.height);
        }

        // 第二步：按实际绘制区域重建布局（应用对齐/下划线/删除线）并绘制
        this->createText(rect1, this->format1.Get(), this->layout1, primaryText, config.align_primary, config.underline_primary, config.strikethrough_primary);
        this->drawText(rect1, this->layout1.Get(), primaryColor);
        if (hasSecondary) {
            this->createText(rect2, this->format2.Get(), this->layout2, config.lyric_secondary, config.align_secondary, config.underline_secondary, config.strikethrough_secondary);
            this->drawText(rect2, this->layout2.Get(), config.color_secondary);
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
