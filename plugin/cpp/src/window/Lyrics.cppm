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

        const auto [pxW, pxH] = this->render->GetSize();
        // 单位换算（配合 Renderer 的 SetDpi 修正）：GetSize 返回物理像素，而
        // DWrite/D2D 绘制坐标是 DIP（= 物理像素 × 96/dpi）。旧代码在默认
        // 96DPI 下两者数值巧合相等；修正渲染目标 DPI 后必须换算，否则
        // 布局高度按物理像素算会溢出 DIP 实际可用高（文字被 clip/居中错位）。
        float dpiX = 96.f, dpiY = 96.f;
        this->render->GetDpi(&dpiX, &dpiY);
        if (dpiX <= 0.f) { dpiX = 96.f; }
        if (dpiY <= 0.f) { dpiY = 96.f; }
        const auto width = static_cast<float>(pxW) * 96.f / dpiX;
        const auto height = static_cast<float>(pxH) * 96.f / dpiY;
        // 字号语义保持“物理像素”（与旧版一致，用户预期不变）：旧版渲染目标
        // 恒 96DPI，size=14 即 14 物理像素行高；SetDpi 修正后 14 DIP 会变成
        // 17.5 物理像素并触发缩小自适应（用户反馈“字好小”，实测行高 15px→12px）。
        // 这里把 size_* 按 96/dpi 折算成 DIP，物理大小回到旧版，清晰度享受原生栅格化。
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
        // 取 taskbarFrame 高度），故此处直接以窗口高为目标：
        // - 带翻译（两行）：每行分到 height/2；
        // - 不带翻译（单行）：占满整个 height。
        // 旧逻辑只在超出时缩小（scale<1），固定 14 号在 40px 栏上永远偏小（用户
        // 反馈“字好小”）；现在改为双向贴合：不足则放大、超出则缩小（保留 0.6
        // 下限保护缩小极端），主/副字号比例保持 config 设定不变。
        auto scale = 1.0f;
        const auto totalHeight = this->metrics1.height + this->metrics2.height;
        if (totalHeight > 0.0f) {
            // 用户方案（贴合栏高）：带翻译每行 em ≈ 栏高/2，单行 em ≈ 栏高。
            // DWrite 自然行框 ≈1.32em，纯自然贴合（scale=height/total）只能做到
            // em≈0.76×栏高/行，视觉仍偏小（用户反馈“还是很小”）。改为目标总高
            // = 1.18×栏高：em ≈ 0.9×栏高/行，上下各溢出 ~9% 由窗口边缘对称裁切
            // （CJK 主笔画在基线上方 0.88em 内不受影响；拉丁下伸部如 g/j 可能
            // 被轻微裁切，属最大化字号的既定取舍）。
            scale = 1.18f * height / totalHeight;
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
