#ifndef EXAMPLES_POLLUXDESK_FILES_FILES_FORM_H_
#define EXAMPLES_POLLUXDESK_FILES_FILES_FORM_H_

// dui
#include "dui/dui.h"

#include <vector>

/** PolluxOS file manager (pure code mode, no layout XML, Wayland/wlroots).
 *
 *  A Finder-style light-themed file browser window. The wlroots compositor
 *  draws the macOS-style titlebar and traffic lights; this client only paints
 *  the toolbar, the scrollable entry list and the status bar.
 *
 *  Interactions:
 *    - single click on a directory row enters it
 *    - single click on a file opens it (text files in wayst+vim,
 *      executables are run directly)
 *    - toolbar: back / up / home / open terminal here / refresh
 */
class FilesForm : public ui::WindowImplBase
{
    typedef ui::WindowImplBase BaseClass;
public:
    FilesForm();
    virtual ~FilesForm() override;

    /** Resource-related interfaces: pure code mode, no layout XML is loaded. */
    virtual DString GetSkinFolder() override;
    virtual DString GetSkinFile() override;

    /** Window creation attributes (no caption/shadow: compositor draws them). */
    virtual void GetCreateWindowAttributes(ui::WindowCreateAttributes& attrs) override;

    /** Called after the window is created, for subclasses to do some initialization work */
    virtual void OnInitWindow() override;

    /** Directory to open first.  Set before the window is created; empty (the
     *  default) starts in the home directory, and a value that cannot be read
     *  falls back there rather than leaving an empty window. */
    void SetStartDir(const DString& dir) { m_startDir = dir; }

private:
    /** One row of the directory listing. */
    struct Entry
    {
        DString name;    // base name
        bool isDir;
        long long size;  // bytes (0 for directories)
        DString mtime;   // "yyyy-MM-dd HH:mm"
    };

    void BuildUi();
    void BuildToolbar(ui::VBox* pRoot);
    void BuildFileList(ui::VBox* pRoot);
    void BuildStatusBar(ui::VBox* pRoot);

    /** @return false when the directory could not be listed. */
    bool Navigate(const DString& path);
    void NavigateBack();
    void NavigateForward();
    void Refresh();
    void ReloadList();
    void UpdatePathLabel();
    void OpenEntry(const Entry& entry);

    /** fork/exec a shell command without waiting for it. */
    void LaunchCommand(const DString& cmdline);

    static bool ListDirectory(const DString& dir, std::vector<Entry>& out);
    static void ShellQuote(DString& arg);   // wrap into '...' for /bin/sh

    std::vector<Entry> m_entries;
    std::vector<DString> m_history;   // visited dirs; last = previous dir
    std::vector<DString> m_forward;   // dirs popped by NavigateBack
    DString m_curDir;
    DString m_startDir;   // requested on the command line; may be empty

    ui::VScrollBox* m_pFileList = nullptr;
    ui::Label* m_pPathLabel = nullptr;
    ui::Label* m_pStatusLabel = nullptr;
};

#endif // EXAMPLES_POLLUXDESK_FILES_FILES_FORM_H_
