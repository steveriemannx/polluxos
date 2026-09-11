#ifndef EXAMPLES_POLLUXDESK_POLLUXDESK_FORM_H_
#define EXAMPLES_POLLUXDESK_POLLUXDESK_FORM_H_

// dui
#include "dui/dui.h"

#include <memory>

class LaunchPadForm;

/** PolluxOS desktop shell (pure code mode, no layout XML, Wayland/wlroots).
 *
 *  A wlroots-based desktop environment styled after macOS Big Sur / Sonoma
 *  (light appearance):
 *    - Big Sur style light gradient wallpaper (drawn by the wlroots
 *      compositor, not by this client)
 *    - translucent macOS-style top menu bar with app/menu dropdowns and clock
 *    - centered translucent icon-only dock with colored app tiles
 *
 *  Window chrome is deliberately absent from this client: the compositor
 *  owns every app window titlebar, traffic light and drop shadow.
 *  The shell is a fullscreen borderless window pinned behind apps and is
 *  launched by the wlroots compositor via the
 *  polluxdesk-compositor-desktop session script.
 */
class PolluxOSForm : public ui::WindowImplBase
{
    typedef ui::WindowImplBase BaseClass;
public:
    PolluxOSForm();
    virtual ~PolluxOSForm() override;

    /** Resource-related interfaces: pure code mode, no layout XML is loaded. */
    virtual DString GetSkinFolder() override;
    virtual DString GetSkinFile() override;

    /** Window creation attributes (fullscreen, no caption, no shadow). */
    virtual void GetCreateWindowAttributes(ui::WindowCreateAttributes& attrs) override;

    /** Called after the window is created, for subclasses to do some initialization work */
    virtual void OnInitWindow() override;

    /** A single native menu entry. `cmd == nullptr` + enabled == false means a
     *  disabled item; `separator` renders a hairline divider. */
    struct MenuItem
    {
        const char* text;
        const char* cmd;      // shell command; nullptr for disabled entries
        bool        separator;
        bool        enabled;
    };

private:
    void BuildUi();
    void BuildMenuBar(ui::VBox* pRoot);
    void BuildDesktopArea(ui::VBox* pRoot);
    void BuildDock(ui::VBox* pRoot);

    void StartClock();
    void UpdateClock();

    void LaunchApp(const char* cmdline);

    void ToggleMenu(int menuIndex);
    void ShowMenuPanel(const MenuItem* items, int count, int x, int y);
    void HideMenuPanel();
    void SetMenuButtonActive(int index, bool active);
    void ShowQuickMenu(int x, int y);

    // Launchpad: a separate always-on-top overlay window (see LaunchPadForm)
    // so the app grid is never buried under other app windows.
    void ShowLaunchPad();
    void HideAppPanel();
    bool IsAppPanelVisible() const;

    void AttachMenuDismissHandlers();
    bool IsPointInMenuButton(const ui::UiPoint& pt) const;
    bool ShouldMenuStayOpenForMove(const ui::UiPoint& pt) const;
    bool ShouldMenuStayOpenForPress(const ui::UiPoint& pt) const;

    // Menu bar content (macOS style).
    static const MenuItem kAppMenu[];
    static const MenuItem kFileMenu[];
    static const MenuItem kEditMenu[];
    static const MenuItem kViewMenu[];
    static const MenuItem kGoMenu[];
    static const MenuItem kWindowMenu[];
    static const MenuItem kHelpMenu[];
    static const MenuItem* kMenuBarMenus[];
    static const int kMenuBarMenuCounts[];

    // Desktop right-click quick actions.
    static const MenuItem kQuickMenu[];
    static const int kQuickMenuCount;

    // Dock launcher commands (client-side launchers for the wlroots
    // compositor desktop).
    static const struct DockApp
    {
        const char* label;   // Chinese name shown under the icon
        const char* glyph;   // icon glyph (ASCII/CJK, rendered in the embedded fonts)
        const char* icon;    // embedded SVG icon path, when available
        const char* color;   // macOS-like icon color ("#RRGGBB", gradient base)
        const char* color2;  // gradient end color
        const char* cmd;     // shell command
    } kDockApps[];

    ui::VBox*    m_pMenuPanel = nullptr;
    ui::HBox*    m_pMenuBar = nullptr;
    ui::Button*  m_pAppButton = nullptr;
    ui::Button*  m_menuButtons[6] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
    int          m_openMenuIndex = -1;

    LaunchPadForm* m_pLaunchPad = nullptr;   // overlay app-grid window
    std::weak_ptr<ui::WeakFlag> m_launchPadWeak;  // lifetime of m_pLaunchPad

    ui::Label* m_pClockLabel = nullptr;
    ui::Label* m_pDesktopClockLabel = nullptr;
    ui::Label* m_pDesktopDateLabel = nullptr;
    size_t     m_clockTimerId = 0;
};

#endif // EXAMPLES_POLLUXDESK_POLLUXDESK_FORM_H_
