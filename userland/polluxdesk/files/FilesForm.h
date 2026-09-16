#ifndef EXAMPLES_POLLUXDESK_FILES_FILES_FORM_H_
#define EXAMPLES_POLLUXDESK_FILES_FILES_FORM_H_

// dui
#include "dui/dui.h"

#include <vector>

/** PolluxOS file manager (pure code mode, no layout XML, Wayland/wlroots).
 *
 *  Modelled on the two browsers people actually compare things to: Windows
 *  Explorer and GNOME Files. That means a places sidebar, a breadcrumb path,
 *  a sortable column header, a status bar, and a right-click menu on a row --
 *  the parts that make a file list into a file browser.
 *
 *  Interactions:
 *    - single click selects a row, double click opens it
 *    - clicking a column heading sorts by it, and again reverses it
 *    - right click opens a menu for that row (open / terminal here / move to
 *      the trash)
 *    - the toolbar walks the history, goes up, makes a folder and switches
 *      between the list and the icon grid
 */
class FilesForm : public ui::WindowImplBase
{
    typedef ui::WindowImplBase BaseClass;
public:
    FilesForm();
    virtual ~FilesForm() override;

    /** Resource-related interfaces: pure code mode, no layout XML is loaded. */
    virtual U8String GetSkinFolder() override;
    virtual U8String GetSkinFile() override;

    /** Window creation attributes (no caption/shadow: compositor draws them). */
    virtual void GetCreateWindowAttributes(ui::WindowCreateAttributes& attrs) override;

    /** Called after the window is created, for subclasses to do some initialization work */
    virtual void OnInitWindow() override;

    /** Directory to open first.  Set before the window is created; empty (the
     *  default) starts in the home directory, and a value that cannot be read
     *  falls back there rather than leaving an empty window. */
    void SetStartDir(const U8String& dir) { m_startDir = dir; }

private:
    /** One row of the directory listing. */
    struct Entry
    {
        U8String name;      // base name
        bool isDir;
        long long size;    // bytes (0 for directories)
        U8String mtime;     // "yyyy-MM-dd HH:mm"; sorts as it reads
    };

    /** A place in the sidebar. */
    struct Place
    {
        const char* label;
        U8String path;
        const char* icon;   // which drawn glyph: folder / house / trash / disk
    };

    // ---- construction ----------------------------------------------------
    void BuildUi();
    void BuildToolbar(ui::VBox* pRoot);
    void BuildBody(ui::VBox* pRoot);
    void BuildSidebar(ui::HBox* pParent);
    void BuildContent(ui::HBox* pParent);
    void BuildStatusBar(ui::VBox* pRoot);
    void BuildContextMenu();

    // ---- navigation ------------------------------------------------------
    /** @return false when the directory could not be listed. */
    bool Navigate(const U8String& path);
    void NavigateBack();
    void NavigateForward();
    void NavigateUp();
    void Refresh();

    // ---- content ---------------------------------------------------------
    void ReloadList();
    void UpdateBreadcrumb();
    void UpdateStatus();
    void OpenEntry(const Entry& entry);
    void SelectRow(size_t index, unsigned int modifiers);
    void OpenRow(size_t index);
    void ShowEntryMenu(size_t index, const ui::UiPoint& pt);
    void HideContextMenu();
    void TrashEntry(size_t index);
    void NewFolder();
    void SetIconView(bool icons);

    void ShowProperties(size_t index);
    bool IsSelected(size_t index) const;

    /** fork/exec a shell command without waiting for it. */
    void LaunchCommand(const U8String& cmdline);

    static bool ListDirectory(const U8String& dir, std::vector<Entry>& out);
    static void ShellQuote(U8String& arg);   // wrap into '...' for /bin/sh

    std::vector<Entry> m_entries;
    std::vector<U8String> m_history;   // visited dirs; last = previous dir
    std::vector<U8String> m_forward;   // dirs popped by NavigateBack
    U8String m_curDir;
    U8String m_startDir;

    // View state.
    int  m_sortColumn = 0;        // 0 = name, 1 = size, 2 = modified
    bool m_sortAscending = true;
    bool m_iconView = false;

    // What is selected, by name. The listing is sorted on every reload, so a
    // row index would point at whatever the sort moved into that position --
    // only the name stays with the file the click landed on.
    std::vector<U8String> m_selected;
    U8String m_anchor;                 // last clicked, for shift-click ranges

    ui::VScrollBox* m_pFileList = nullptr;
    ui::HBox*       m_pHeaderRow = nullptr;
    ui::HBox*       m_pBreadcrumb = nullptr;
    ui::Label*      m_pStatusLabel = nullptr;
    ui::Label*      m_pSelectionLabel = nullptr;
    ui::VBox*       m_pSidebar = nullptr;

    ui::Button*     m_pBackButton = nullptr;
    ui::Button*     m_pForwardButton = nullptr;
    // Kept so the sorted column can carry its arrow.
    ui::Button*     m_pColumnButtons[3] = { nullptr, nullptr, nullptr };

    // The breadcrumb is a fixed row of slots rather than one button per path
    // segment. dui lays a control out when the layout pass runs, and adding
    // children to a live box does not re-run it -- so segments created on the
    // way into a directory would exist but never get a size. These are built
    // once at their final width and navigation only changes their text.
    static const int kCrumbSlots = 4;
    ui::Button* m_pCrumbButtons[kCrumbSlots] = { nullptr, nullptr, nullptr,
                                                 nullptr };
    U8String     m_crumbTargets[kCrumbSlots];
    // The "›" in front of each slot but the first, so a path that ends early
    // can take its separators with it.
    std::vector<ui::Label*> m_pCrumbSeps;
    // Control, not Button: the rows are ButtonHBox so they can hold an icon
    // and a label, and that is a different type. The paths sit alongside
    // rather than being read back off a tooltip.
    std::vector<ui::Control*> m_pPlaceButtons;
    std::vector<U8String>     m_placePaths;

    // The right-click menu: a floating panel inside the window, the same
    // technique the desktop shell uses for its dropdowns.
    ui::VBox* m_pContextMenu = nullptr;
    size_t    m_contextIndex = 0;
};

#endif // EXAMPLES_POLLUXDESK_FILES_FILES_FORM_H_
