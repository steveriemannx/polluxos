#ifndef EXAMPLES_POLLUXDESK_POLLUXDESK_FORM_H_
#define EXAMPLES_POLLUXDESK_POLLUXDESK_FORM_H_

// dui
#include "dui/dui.h"

#include "PolluxSettings.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

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
    explicit PolluxOSForm(bool dockOverlay = false,
                          PolluxOSForm* desktopOwner = nullptr,
                          bool menuOverlay = false);
    virtual ~PolluxOSForm() override;

    /** Resource-related interfaces: pure code mode, no layout XML is loaded. */
    virtual U8String GetSkinFolder() override;
    virtual U8String GetSkinFile() override;

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

    /** Tear the control tree down and build it again so layout is recomputed
     *  from scratch. Only ever called from the clock timer: AttachBox
     *  deletes the previous tree synchronously, which would be a
     *  use-after-free inside an event handler. */
    void RebuildUi();

    /** Fill the drawing palette from m_settings. Runs before every build, so
     *  the tree is built with the colours it should be seen in. */
    void ApplyAppearance();

    /** Watch the settings file and rebuild when it changes. Polled on the
     *  clock timer like the window state: no signal, no protocol, and editing
     *  the file by hand works exactly the same way. */
    void PollSettings();
    void BuildMenuBar(ui::VBox* pRoot);
    void BuildDesktopArea(ui::VBox* pRoot);
    void BuildDock(ui::VBox* pRoot);
    void SetCompositorMenuOpen(bool open, int menuHeight = 0);
    void UpdateDockHitArea();
    void SetDockCompositorTitle();
    void RequestRestoreWindow(unsigned long id);

    void StartClock();
    void UpdateClock();

    /** One window as reported by the compositor's state file. */
    struct WindowInfo
    {
        unsigned long id = 0;
        std::string   appId;
        std::string   title;
        bool          minimized = false;
        bool          focused = false;
        int           width = 0;
        int           height = 0;
        long          pid = 0;
        std::string   exe;   // resolved from pid
    };

    /** Re-read the compositor's window list if it has been rewritten since
     *  the last look, and repaint the dock's running indicators if the set
     *  of running applications changed. */
    void PollWindowState();
    void ApplyRunningIndicators();
    bool IsAppRunning(const char* exeList) const;

    /** True when one of the pinned tiles already stands for this program. */
    static bool HasPinnedTile(const std::string& exe);

    /** Rebuild the dock's running-application tiles when the set of programs
     *  without a pinned tile changes. Like the shelf, this defers the rebuild
     *  to the next timer tick rather than doing it where the change was
     *  noticed. */
    void UpdateRunningApps();

    /** A dock tile's right-click menu: the window title, then the two ways to
     *  end the program -- `pids` is what would go after `kill`. */
    void ShowDockMenu(const std::string& title, const std::string& pids, int x, int y);

    /** Repopulate the shelf of minimized windows in the dock. Chips are
     *  created once with the dock and only shown, hidden and relabelled, so
     *  a window being minimized never rebuilds the shell. */
    void UpdateMinimizedShelf();

    /** Capture the window the compositor is holding on screen for us and
     *  tell it the pixels are safely in a file. */
    void GrabPendingThumbnail();

    void LaunchApp(const char* cmdline);

    void ToggleMenu(int menuIndex);
    void ShowMenuPanel(const MenuItem* items, int count, int x, int y);
    void HideMenuPanel();
    void SetMenuButtonActive(int index, bool active);
    void ShowQuickMenu(int x, int y);

    /** A dropdown that is not a menu. The right-hand end of the menu bar is
     *  not decoration -- each item opens a panel of real controls, the way it
     *  does on macOS. `build` fills the panel and returns its height. */
    void ShowPopover(int x, int y, int width,
                     const std::function<int(ui::VBox* panel)>& build);
    void ShowControlCentre(int x, int y);
    void ShowVolumePanel(int x, int y);
    void ShowWifiPanel(int x, int y);
    void ShowBatteryPanel(int x, int y);

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
        const char* exe;     // executable names that light the running dot,
                             // "|"-separated; empty for tiles that never do
        int         group;   // dock section; a divider is drawn between groups
    } kDockApps[];

    ui::VBox*    m_pMenuPanel = nullptr;
    ui::HBox*    m_pMenuBar = nullptr;
    ui::Button*  m_pAppButton = nullptr;
    ui::Button*  m_menuButtons[6] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
    int          m_openMenuIndex = -1;

    LaunchPadForm* m_pLaunchPad = nullptr;   // overlay app-grid window
    std::weak_ptr<ui::WeakFlag> m_launchPadWeak;  // lifetime of m_pLaunchPad

    // Read from disk and reloaded when the settings file changes; the dock,
    // the menus and everything else that follows the appearance read it here
    // rather than re-reading the file per control.
    pollux::Settings m_settings;
    bool m_dockOverlay = false;
    PolluxOSForm* m_desktopOwner = nullptr;
    bool m_menuOverlay = false;
    ui::HBox* m_pDockBar = nullptr;
    ui::UiRect m_dockHitArea;
    int m_dockHitRadius = 0;
    bool m_dockHitAreaValid = false;
    bool m_dockMenuOpen = false;
    std::vector<ui::Control*> m_dockDotAnchors;

    // Window state published by the compositor, refreshed on the clock timer.
    // Polled rather than signalled: the shell already ticks once a second and
    // that is well below the threshold where a running indicator looks late.
    std::vector<WindowInfo> m_windows;
    unsigned long long m_stateMtime = 0;
    unsigned long long m_settingsMtime = 0;
    // Indicator dot per dock tile, index-aligned with kDockApps. The controls
    // persist across state changes; only their colour is repainted, so an app
    // starting never rebuilds the dock.
    std::vector<ui::Control*> m_dockDots;

    // Minimized windows: a chip each, between the separator and the trash.
    // Clicking one asks the compositor to bring that window back.
    static const int kMinimizedSlots = 4;
    std::vector<ui::Button*>   m_minimizedSlots;
    std::vector<unsigned long> m_minimizedIds;   // window id per slot; 0 = empty
    std::vector<unsigned long> m_shelfIds;       // last applied set

    // Programs running without a launcher of their own, drawn as tiles of
    // their own in the dock. Built by BuildDock(), so a change to the set
    // means a rebuild -- hence the last-applied copy, compared against.
    static const int kRunningTileSlots = 8;
    std::vector<unsigned long> m_runningIds;
    // The restore request travels as the window title; the marker is cleared
    // on the next timer tick rather than immediately, because two SetText
    // calls in the same handler can coalesce into one commit and the
    // compositor would never see the request.
    bool m_titleMarkerPending = false;

    // Published by the compositor while it keeps a just-minimized window on
    // screen: the rectangle to grab before it is hidden.
    bool          m_thumbPending = false;
    unsigned long m_thumbId = 0;
    int           m_thumbX = 0, m_thumbY = 0, m_thumbW = 0, m_thumbH = 0;
    // Set when something the dock has to draw differently has changed, so the
    // rebuild happens on the next timer tick rather than where it was noticed.
    bool m_uiDirty = false;
    // Window-level dismiss callbacks append rather than replace, so attaching
    // them once per rebuild would stack duplicates and close menus instantly.
    bool m_handlersAttached = false;

    ui::Label* m_pClockLabel = nullptr;
    ui::Label* m_pBatteryLabel = nullptr;   // percentage beside the battery glyph
    ui::Button* m_pVolumeButton = nullptr;      // status items; each opens a panel
    ui::Button* m_pWifiButton = nullptr;
    ui::Button* m_pBatteryButton = nullptr;
    ui::Label* m_pDesktopClockLabel = nullptr;
    ui::Label* m_pDesktopDateLabel = nullptr;
    size_t     m_clockTimerId = 0;
};

#endif // EXAMPLES_POLLUXDESK_POLLUXDESK_FORM_H_
