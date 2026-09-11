#ifndef EXAMPLES_POLLUXDESK_LAUNCHPAD_FORM_H_
#define EXAMPLES_POLLUXDESK_LAUNCHPAD_FORM_H_

// dui
#include "dui/dui.h"

#include <functional>
#include <vector>

/** PolluxOS Launchpad (pure code mode, no layout XML, Wayland/wlroots).
 *
 *  A separate borderless overlay WINDOW (not an in-shell panel) titled
 *  "PolluxOS Launchpad": the wlroots compositor keeps that title always on
 *  top of regular app windows, so the app grid is never buried underneath
 *  terminals the way the old in-shell panel was.
 *
 *  - landscape frosted grid of .desktop applications (8 columns)
 *  - clicking a tile launches the app and hides the pad
 *  - Esc or losing keyboard focus also hides it
 */
class LaunchPadForm : public ui::WindowImplBase
{
    typedef ui::WindowImplBase BaseClass;
public:
    LaunchPadForm();
    virtual ~LaunchPadForm() override;

    /** Resource-related interfaces: pure code mode, no layout XML is loaded. */
    virtual DString GetSkinFolder() override;
    virtual DString GetSkinFile() override;

    /** Window creation attributes (borderless overlay, no shadow). */
    virtual void GetCreateWindowAttributes(ui::WindowCreateAttributes& attrs) override;

    /** Called after the window is created, for subclasses to do some initialization work */
    virtual void OnInitWindow() override;

    /** One scanned .desktop application entry. */
    struct DesktopApp
    {
        DString name;   // display name (Name= from the .desktop file)
        DString exec;   // command line (Exec=, field codes stripped)
        DString icon;   // embedded SVG path or resolved system icon
    };

    /** Scan the freedesktop application directories. */
    static void ScanDesktopApps(std::vector<DesktopApp>& apps);

    /** `cmd == nullptr + enabled == false`-style callback: invoked with the
     *  Exec line when a tile is clicked (the owner launches + hides). */
    void SetLaunchHandler(std::function<void(const DString& cmd)> handler);

    /** Re-scan applications, rebuild the grid and size/center the window. */
    void RefreshAndShow();

    /** Hide (unmap) the overlay; shown again via RefreshAndShow(). */
    void HidePad();

private:
    void BuildUi();

    static const int kColumns = 8;        // grid columns
    static const int kMaxVisibleRows = 6; // rows shown per page

    std::vector<DesktopApp> m_apps;
    std::function<void(const DString&)> m_launchHandler;
    ui::VBox* m_pCard = nullptr;         // frosted panel card
    ui::VScrollBox* m_pGrid = nullptr;   // scrollable app-grid container
    ui::HBox* m_pNav = nullptr;          // pager row (prev / n / next)
    int m_firstRow = 0;                  // first visible grid row (pager)
    size_t m_totalRows = 0;
    bool m_shown = false;   // guards against KillFocus during creation
    bool m_closing = false; // guards against recursive close
};

#endif // EXAMPLES_POLLUXDESK_LAUNCHPAD_FORM_H_
