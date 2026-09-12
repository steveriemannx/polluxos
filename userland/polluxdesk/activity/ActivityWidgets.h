#ifndef EXAMPLES_POLLUXDESK_ACTIVITY_ACTIVITY_WIDGETS_H_
#define EXAMPLES_POLLUXDESK_ACTIVITY_ACTIVITY_WIDGETS_H_

// The two controls the activity window paints itself.
//
// Both exist for the same reason: dui lays a control out once, so anything
// whose size has to follow a reading -- a bar that grows, a chart that scrolls
// -- cannot be built out of ordinary boxes.  A control that draws its own
// contents in Paint() can change shape as often as it likes, because nothing
// about it depends on the layout pass.

#include "dui/dui.h"

#include <vector>

namespace activity {

/** A reading's history, drawn as an area chart: the newest sample sits at the
 *  right edge and older ones slide left. */
class HistoryGraph : public ui::Control
{
    typedef ui::Control BaseClass;
public:
    explicit HistoryGraph(ui::Window* pWindow);

    /** @param values  oldest first, in the same unit as @p scale
     *  @param scale   the value that fills the full height */
    void SetSamples(const std::vector<float>& values, float scale);

    void SetColours(const DString& line, const DString& fillTop,
                    const DString& fillBottom, const DString& grid);

    /** Highest sample currently drawn, in the same unit as the scale. */
    float Peak() const { return m_peak; }

    virtual void Paint(ui::IRender* pRender, const ui::UiRect& rcPaint) override;

private:
    std::vector<float> m_values;
    float m_scale = 100.0f;
    float m_peak = 0.0f;
    ui::UiColor m_line, m_fillTop, m_fillBottom, m_grid;
};

/** One reading as a rounded track with a filled part. */
class MeterBar : public ui::Control
{
    typedef ui::Control BaseClass;
public:
    explicit MeterBar(ui::Window* pWindow);

    /** @param fraction 0..1; values outside are clamped. */
    void SetFraction(float fraction);
    void SetColours(const DString& track, const DString& fill);

    virtual void Paint(ui::IRender* pRender, const ui::UiRect& rcPaint) override;

private:
    float m_fraction = 0.0f;
    ui::UiColor m_track, m_fill;
};

}  // namespace activity

#endif  // EXAMPLES_POLLUXDESK_ACTIVITY_ACTIVITY_WIDGETS_H_
