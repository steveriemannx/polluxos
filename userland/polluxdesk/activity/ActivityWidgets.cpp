#include "ActivityWidgets.h"

#include <algorithm>
#include <memory>

namespace activity {

namespace {
// One slot per sample.  The window keeps its history this long, so the chart
// and the history agree on how far back "the last two minutes" reaches.
const int kSlots = 120;
}  // namespace

// ---------------------------------------------------------------------------
// HistoryGraph
// ---------------------------------------------------------------------------
HistoryGraph::HistoryGraph(ui::Window* pWindow)
    : ui::Control(pWindow)
{
    SetMouseEnabled(false);
}

void HistoryGraph::SetSamples(const std::vector<float>& values, float scale)
{
    m_values = values;
    if (scale > 0.0f) {
        m_scale = scale;
    }
    m_peak = 0.0f;
    for (float value : m_values) {
        m_peak = (std::max)(m_peak, value);
    }
    Invalidate();
}

void HistoryGraph::SetColours(const U8String& line, const U8String& fillTop,
                              const U8String& fillBottom, const U8String& grid)
{
    m_line = GetUiColor(line);
    m_fillTop = GetUiColor(fillTop);
    m_fillBottom = GetUiColor(fillBottom);
    m_grid = GetUiColor(grid);
    Invalidate();
}

void HistoryGraph::Paint(ui::IRender* pRender, const ui::UiRect& rcPaint)
{
    BaseClass::Paint(pRender, rcPaint);

    const ui::UiRect rc = GetRect();
    if (rc.IsEmpty() || rc.Width() < 8 || rc.Height() < 12) {
        return;
    }
    const float left = static_cast<float>(rc.left);
    const float right = static_cast<float>(rc.right);
    const float top = static_cast<float>(rc.top);
    const float bottom = static_cast<float>(rc.bottom);
    const float height = bottom - top;

    // Quarter lines: enough to read a value off the chart, faint enough not to
    // compete with the line itself.
    for (int i = 1; i <= 3; ++i) {
        const float y = top + height * static_cast<float>(i) / 4.0f;
        pRender->DrawLine(ui::UiPointF(left, y), ui::UiPointF(right, y), m_grid, 1.0f);
    }

    if (m_values.size() < 2 || m_scale <= 0.0f) {
        return;
    }

    // Samples are placed one slot apart from the right edge, so a chart that
    // has just started fills up from the right rather than stretching two
    // points across the whole width.
    const float step = (right - left) / static_cast<float>(kSlots - 1);
    const size_t count = (std::min)(m_values.size(), static_cast<size_t>(kSlots));
    const size_t first = m_values.size() - count;

    std::vector<ui::UiPointF> line;
    line.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        float value = m_values[first + i];
        if (value < 0.0f) value = 0.0f;
        float y = bottom - height * (value / m_scale);
        if (y < top + 1.0f) y = top + 1.0f;
        if (y > bottom) y = bottom;
        const float x = right - static_cast<float>(count - 1 - i) * step;
        line.push_back(ui::UiPointF(x, y));
    }

    // The filled area is the line closed along the bottom edge.
    std::vector<ui::UiPointF> area = line;
    area.push_back(ui::UiPointF(right, bottom));
    area.push_back(ui::UiPointF(line.front().x, bottom));

    ui::IRenderFactory* factory = ui::GlobalManager::Instance().GetRenderFactory();
    if (factory != nullptr) {
        std::unique_ptr<ui::IPath> path(factory->CreatePath());
        if (path != nullptr) {
            path->AddPolygon(area.data(), static_cast<int32_t>(area.size()));
            // Direction 2 is top to bottom, which is what fades the fill out
            // towards the axis.
            pRender->FillPath(path.get(), ui::UiRectF(left, top, right, bottom),
                              m_fillTop, m_fillBottom, 2);
        }
    }

    for (size_t i = 1; i < line.size(); ++i) {
        pRender->DrawLine(line[i - 1], line[i], m_line, 1.5f);
    }
}

// ---------------------------------------------------------------------------
// MeterBar
// ---------------------------------------------------------------------------
MeterBar::MeterBar(ui::Window* pWindow)
    : ui::Control(pWindow)
{
    SetMouseEnabled(false);
}

void MeterBar::SetFraction(float fraction)
{
    if (fraction < 0.0f) fraction = 0.0f;
    if (fraction > 1.0f) fraction = 1.0f;
    // Repainting for a change nobody can see would be wasted work on a control
    // that is updated once a second.
    if (fraction > m_fraction ? fraction - m_fraction < 0.004f
                              : m_fraction - fraction < 0.004f) {
        return;
    }
    m_fraction = fraction;
    Invalidate();
}

void MeterBar::SetColours(const U8String& track, const U8String& fill)
{
    m_track = GetUiColor(track);
    m_fill = GetUiColor(fill);
    Invalidate();
}

void MeterBar::Paint(ui::IRender* pRender, const ui::UiRect& rcPaint)
{
    BaseClass::Paint(pRender, rcPaint);

    const ui::UiRect rc = GetRect();
    if (rc.IsEmpty()) {
        return;
    }
    const float radius = static_cast<float>(rc.Height()) / 2.0f;
    pRender->FillRoundRect(
        ui::UiRectF(static_cast<float>(rc.left), static_cast<float>(rc.top),
                    static_cast<float>(rc.right), static_cast<float>(rc.bottom)),
        radius, radius, m_track);

    if (m_fraction <= 0.0f) {
        return;
    }
    float width = static_cast<float>(rc.Width()) * m_fraction;
    // A part-filled bar should still read as a pill rather than as a sliver.
    if (width < static_cast<float>(rc.Height())) {
        width = static_cast<float>(rc.Height());
    }
    if (width > static_cast<float>(rc.Width())) {
        width = static_cast<float>(rc.Width());
    }
    pRender->FillRoundRect(
        ui::UiRectF(static_cast<float>(rc.left), static_cast<float>(rc.top),
                    static_cast<float>(rc.left) + width, static_cast<float>(rc.bottom)),
        radius, radius, m_fill);
}

}  // namespace activity
