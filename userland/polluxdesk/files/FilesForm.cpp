#include "FilesForm.h"

#include <ctime>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>

namespace {

// macOS Big Sur / Sonoma light palette (matches the PolluxOS shell).
const DString kBarBg        = _T("#F2FFFFFF");
const DString kBarBorder    = _T("#33000000");
const DString kTransparent  = _T("#00000000");
const DString kListBg       = _T("#F9FFFFFF");
const DString kRowHot       = _T("#220A84FF");
const DString kTextDark     = _T("#FF1D1D1F");
const DString kTextBody     = _T("#FF3A3A3C");
const DString kTextHint     = _T("#FF8E8E93");
const DString kAccent       = _T("#FF0A84FF");
const DString kFolderColor  = _T("#FF4C9FDB");

DString FormatSize(long long bytes)
{
    char buf[64];
    if (bytes >= 1024LL * 1024LL * 1024LL) {
        std::snprintf(buf, sizeof(buf), "%.1f GB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024LL * 1024LL) {
        std::snprintf(buf, sizeof(buf), "%.1f MB",
                      static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes >= 1024) {
        std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%lld B", bytes);
    }
    return DString(buf);
}

// Extensions opened in a terminal text editor; everything else also falls
// back to the editor (there is no MIME handler database on this minimal
// system yet), except executables which are run directly.
bool IsTextExtension(const DString& name)
{
    const char* exts[] = { ".txt", ".md", ".c", ".cpp", ".h", ".hpp", ".cc",
                           ".py", ".sh", ".conf", ".ini", ".log", ".xml",
                           ".json", ".yaml", ".yml", ".cmake", ".toml",
                           ".desktop", ".css", ".js", ".html", ".s", ".S" };
    for (const char* ext : exts) {
        if (name.size() >= std::strlen(ext) &&
            name.compare(name.size() - std::strlen(ext),
                         std::strlen(ext), ext) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace

FilesForm::FilesForm()
{
    signal(SIGCHLD, SIG_IGN);
}

FilesForm::~FilesForm()
{
}

DString FilesForm::GetSkinFolder()
{
    return _T("");
}

DString FilesForm::GetSkinFile()
{
    // Pure code mode: no layout XML is loaded
    return _T("");
}

void FilesForm::GetCreateWindowAttributes(ui::WindowCreateAttributes& attrs)
{
    attrs.m_bInitSizeDefined = true;
    attrs.m_szInitSize.cx = 960;
    attrs.m_szInitSize.cy = 640;
    attrs.m_bShadowAttached = false;
    attrs.m_bShadowAttachedDefined = true;
    attrs.m_bIsLayeredWindow = true;
    attrs.m_bIsLayeredWindowDefined = true;
    // No caption / resize border: the wlroots compositor draws the macOS-style
    // titlebar and traffic lights above this surface.
    attrs.m_rcCaption = ui::UiRect(0, 0, 0, 0);
    attrs.m_bCaptionDefined = true;
    attrs.m_rcSizeBox = ui::UiRect(0, 0, 0, 0);
    attrs.m_bSizeBoxDefined = true;
    BaseClass::GetCreateWindowAttributes(attrs);
}

void FilesForm::OnInitWindow()
{
    SetShadowAttached(false);
    BuildUi();

    BaseClass::OnInitWindow();

    Navigate(std::getenv("HOME") != nullptr ? DString(std::getenv("HOME"))
                                            : DString(_T("/")));
}

void FilesForm::BuildUi()
{
    ui::VBox* pRoot = new ui::VBox(this);
    pRoot->SetBkColor(kTransparent);
    pRoot->SetBorderColor(kTransparent);
    pRoot->SetAttribute(_T("border_size"), _T("0"));
    pRoot->SetAttribute(_T("padding"), _T("0,0,0,0"));

    BuildToolbar(pRoot);
    BuildFileList(pRoot);
    BuildStatusBar(pRoot);

    AttachBox(pRoot);
}

static void MakeToolButton(ui::Button* pBtn, const DString& text)
{
    pBtn->SetText(text);
    pBtn->SetAttribute(_T("font"), _T("system_14"));
    pBtn->SetAttribute(_T("text_color"), kTextDark);
    pBtn->SetAttribute(_T("text_align"), _T("hcenter,vcenter"));
    pBtn->SetAttribute(_T("height"), _T("30"));
    pBtn->SetAttribute(_T("width"), _T("auto"));
    pBtn->SetAttribute(_T("margin"), _T("4,3,4,3"));
    pBtn->SetAttribute(_T("text_padding"), _T("10,0,10,0"));
    pBtn->SetStateColor(ui::kControlStateNormal, kTransparent);
    pBtn->SetStateColor(ui::kControlStateHot, kRowHot);
    pBtn->SetStateColorRound(ui::kControlStateNormal, ui::UiSize(6, 6), false);
    pBtn->SetStateColorRound(ui::kControlStateHot, ui::UiSize(6, 6), false);
    pBtn->SetAttribute(_T("cursor_type"), _T("hand"));
}

void FilesForm::BuildToolbar(ui::VBox* pRoot)
{
    // Finder-style toolbar: just the back / forward chevrons and the current
    // folder name. (macOS keeps the toolbar minimal; no terminal/refresh.)
    ui::HBox* pBar = new ui::HBox(this);
    pBar->SetAttribute(_T("height"), _T("40"));
    pBar->SetBkColor(kBarBg);
    pBar->SetBorderColor(kBarBorder);
    pBar->SetAttribute(_T("bottom_border_size"), _T("1"));
    pBar->SetAttribute(_T("padding"), _T("8,0,8,0"));
    pRoot->AddItem(pBar);

    ui::Button* pBack = new ui::Button(this);
    MakeToolButton(pBack, _T("‹"));
    pBack->AttachClick([this](const ui::EventArgs& /*args*/) {
        NavigateBack();
        return true;
    });
    pBar->AddItem(pBack);

    ui::Button* pForward = new ui::Button(this);
    MakeToolButton(pForward, _T("›"));
    pForward->AttachClick([this](const ui::EventArgs& /*args*/) {
        NavigateForward();
        return true;
    });
    pBar->AddItem(pForward);

    // Current folder name, Finder style (bold, centered in the toolbar).
    m_pPathLabel = new ui::Label(this);
    m_pPathLabel->SetAttribute(_T("font"), _T("system_bold_16"));
    m_pPathLabel->SetAttribute(_T("text_color"), kTextDark);
    m_pPathLabel->SetAttribute(_T("text_align"), _T("hcenter,vcenter"));
    m_pPathLabel->SetAttribute(_T("width"), _T("stretch"));
    m_pPathLabel->SetText(_T(""));
    m_pPathLabel->SetMouseEnabled(false);
    pBar->AddItem(m_pPathLabel);
}

// Small macOS-style folder icon: a blue gradient rounded square with a
// darker "tab" strip at the top.
static ui::VBox* MakeFolderIcon(ui::Window* pWindow)
{
    ui::VBox* pIcon = new ui::VBox(pWindow);
    pIcon->SetAttribute(_T("width"), _T("26"));
    pIcon->SetAttribute(_T("height"), _T("20"));
    pIcon->SetBkColor(_T("#FF63B0ED"));
    pIcon->SetBkColor2(_T("#FF3E8ECA"));
    pIcon->SetBkColor2Direction(_T("1"));   // left -> right gradient
    pIcon->SetStateColorRound(ui::kControlStateNormal, ui::UiSize(5, 5), false);
    pIcon->SetAttribute(_T("border_round"), _T("5,5"));
    pIcon->SetMouseEnabled(false);

    // The folder tab: a darker rounded strip peeking from the top-left.
    ui::Control* pTab = new ui::Control(pWindow);
    pTab->SetAttribute(_T("width"), _T("12"));
    pTab->SetAttribute(_T("height"), _T("4"));
    pTab->SetAttribute(_T("margin"), _T("-1,-2,0,0"));
    pTab->SetAttribute(_T("halign"), _T("left"));
    pTab->SetBkColor(_T("#FF2F7AB8"));
    pTab->SetMouseEnabled(false);
    pIcon->AddItem(pTab);
    return pIcon;
}

// Small document icon: a white page with faint text lines.
static ui::VBox* MakeFileIcon(ui::Window* pWindow)
{
    ui::VBox* pIcon = new ui::VBox(pWindow);
    pIcon->SetAttribute(_T("width"), _T("18"));
    pIcon->SetAttribute(_T("height"), _T("20"));
    pIcon->SetBkColor(_T("#FFFFFFFF"));
    pIcon->SetBorderColor(_T("#FFC9C9CE"));
    pIcon->SetAttribute(_T("border_size"), _T("1"));
    pIcon->SetStateColorRound(ui::kControlStateNormal, ui::UiSize(3, 3), false);
    pIcon->SetAttribute(_T("border_round"), _T("3,3"));
    pIcon->SetAttribute(_T("child_align"), _T("hcenter,vcenter"));
    pIcon->SetMouseEnabled(false);
    for (int i = 0; i < 3; ++i) {
        if (i > 0) {
            ui::Control* pGap = new ui::Control(pWindow);
            pGap->SetAttribute(_T("height"), _T("2"));
            pGap->SetMouseEnabled(false);
            pIcon->AddItem(pGap);
        }
        ui::Control* pLine = new ui::Control(pWindow);
        pLine->SetAttribute(_T("height"), _T("1"));
        pLine->SetAttribute(_T("width"), _T("10"));
        pLine->SetBkColor(_T("#FFC7C7CC"));
        pLine->SetMouseEnabled(false);
        pIcon->AddItem(pLine);
    }
    return pIcon;
}

void FilesForm::BuildFileList(ui::VBox* pRoot)
{
    // VScrollBox (not plain VBox): only ScrollBox-derived containers support
    // the vscrollbar attribute. Home has 40+ directories, so scrolling is
    // required to reach entries below the fold.
    m_pFileList = new ui::VScrollBox(this);
    m_pFileList->SetAttribute(_T("height"), _T("stretch"));
    m_pFileList->SetBkColor(kListBg);
    m_pFileList->SetAttribute(_T("padding"), _T("6,6,6,6"));
    m_pFileList->SetAttribute(_T("vscrollbar"), _T("true"));
    pRoot->AddItem(m_pFileList);
}

void FilesForm::BuildStatusBar(ui::VBox* pRoot)
{
    ui::HBox* pBar = new ui::HBox(this);
    pBar->SetAttribute(_T("height"), _T("26"));
    pBar->SetBkColor(kBarBg);
    pBar->SetBorderColor(kBarBorder);
    pBar->SetAttribute(_T("top_border_size"), _T("1"));
    pBar->SetAttribute(_T("padding"), _T("10,0,10,0"));
    pRoot->AddItem(pBar);

    m_pStatusLabel = new ui::Label(this);
    m_pStatusLabel->SetAttribute(_T("font"), _T("system_12"));
    m_pStatusLabel->SetAttribute(_T("text_color"), kTextHint);
    m_pStatusLabel->SetAttribute(_T("text_align"), _T("left,vcenter"));
    m_pStatusLabel->SetAttribute(_T("width"), _T("stretch"));
    m_pStatusLabel->SetText(_T(""));
    m_pStatusLabel->SetMouseEnabled(false);
    pBar->AddItem(m_pStatusLabel);
}

bool FilesForm::ListDirectory(const DString& dir, std::vector<Entry>& out)
{
    out.clear();
    DIR* dp = opendir(dir.c_str());
    if (dp == nullptr) {
        return false;
    }
    struct dirent* ent = nullptr;
    while ((ent = readdir(dp)) != nullptr) {
        const char* name = ent->d_name;
        if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) {
            continue;
        }
        Entry entry;
        entry.name = name;
        entry.isDir = (ent->d_type == DT_DIR);
        entry.size = 0;

        DString full = dir;
        if (!full.empty() && full.back() != _T('/')) {
            full += _T('/');
        }
        full += entry.name;

        struct stat st;
        if (stat(full.c_str(), &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                entry.isDir = true;
            } else {
                entry.size = static_cast<long long>(st.st_size);
            }
            std::tm tm_mtime;
            localtime_r(&st.st_mtim.tv_sec, &tm_mtime);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d",
                          tm_mtime.tm_year + 1900, tm_mtime.tm_mon + 1,
                          tm_mtime.tm_mday, tm_mtime.tm_hour, tm_mtime.tm_min);
            entry.mtime = buf;
        }
        out.push_back(entry);
    }
    closedir(dp);

    std::sort(out.begin(), out.end(),
              [](const Entry& a, const Entry& b) {
                  if (a.isDir != b.isDir) {
                      return a.isDir;   // directories first
                  }
                  return a.name < b.name;
              });
    return true;
}

void FilesForm::Navigate(const DString& path)
{
    std::vector<Entry> entries;
    if (!ListDirectory(path, entries)) {
        if (m_pStatusLabel != nullptr) {
            m_pStatusLabel->SetText(_T("无法打开目录：") + path);
            m_pStatusLabel->SetAttribute(_T("text_color"), _T("#FFFF453A"));
        }
        return;
    }
    if (!m_curDir.empty()) {
        m_history.push_back(m_curDir);
    }
    m_forward.clear();
    m_curDir = path;
    m_entries = std::move(entries);
    ReloadList();
}

void FilesForm::NavigateBack()
{
    if (m_history.empty()) {
        return;
    }
    DString prev = m_history.back();
    m_history.pop_back();
    std::vector<Entry> entries;
    if (!ListDirectory(prev, entries)) {
        return;
    }
    if (!m_curDir.empty()) {
        m_forward.push_back(m_curDir);
    }
    m_curDir = prev;
    m_entries = std::move(entries);
    ReloadList();
}

void FilesForm::NavigateForward()
{
    if (m_forward.empty()) {
        return;
    }
    DString next = m_forward.back();
    m_forward.pop_back();
    std::vector<Entry> entries;
    if (!ListDirectory(next, entries)) {
        return;
    }
    if (!m_curDir.empty()) {
        m_history.push_back(m_curDir);
    }
    m_curDir = next;
    m_entries = std::move(entries);
    ReloadList();
}

void FilesForm::Refresh()
{
    std::vector<Entry> entries;
    if (!ListDirectory(m_curDir, entries)) {
        return;
    }
    m_entries = std::move(entries);
    ReloadList();
}

void FilesForm::ReloadList()
{
    if (m_pFileList == nullptr) {
        return;
    }
    UpdatePathLabel();

    m_pFileList->RemoveAllItems();

    int dirCount = 0;
    int fileCount = 0;
    for (size_t i = 0; i < m_entries.size(); ++i) {
        const Entry& entry = m_entries[i];
        entry.isDir ? ++dirCount : ++fileCount;

        // One clickable row: [glyph] name | size | modified time.
        ui::ButtonHBox* pRow = new ui::ButtonHBox(this);
        pRow->SetAttribute(_T("height"), _T("30"));
        pRow->SetAttribute(_T("width"), _T("stretch"));
        pRow->SetAttribute(_T("padding"), _T("6,0,6,0"));
        pRow->SetStateColor(ui::kControlStateNormal, kTransparent);
        pRow->SetStateColor(ui::kControlStateHot, kRowHot);
        pRow->SetStateColorRound(ui::kControlStateNormal, ui::UiSize(6, 6), false);
        pRow->SetStateColorRound(ui::kControlStateHot, ui::UiSize(6, 6), false);
        pRow->SetAttribute(_T("cursor_type"), _T("arrow"));

        const size_t index = i;
        pRow->AttachClick([this, index](const ui::EventArgs& /*args*/) {
            if (index < m_entries.size()) {
                OpenEntry(m_entries[index]);
            }
            return true;
        });

        // macOS Finder-style row icon (blue folder / white document),
        // inside a fixed-width cell so the names align.
        ui::HBox* pIconCell = new ui::HBox(this);
        pIconCell->SetAttribute(_T("width"), _T("36"));
        pIconCell->SetAttribute(_T("height"), _T("stretch"));
        pIconCell->SetAttribute(_T("child_align"), _T("hcenter,vcenter"));
        pIconCell->SetMouseEnabled(false);
        pRow->AddItem(pIconCell);
        if (entry.isDir) {
            pIconCell->AddItem(MakeFolderIcon(this));
        } else {
            pIconCell->AddItem(MakeFileIcon(this));
        }

        ui::Label* pName = new ui::Label(this);
        pName->SetText(entry.name);
        pName->SetAttribute(_T("font"), _T("system_14"));
        pName->SetAttribute(_T("text_color"),
                            entry.isDir ? kAccent : kTextDark);
        pName->SetAttribute(_T("text_align"), _T("left,vcenter"));
        pName->SetAttribute(_T("width"), _T("stretch"));
        pName->SetAttribute(_T("height"), _T("stretch"));
        pName->SetMouseEnabled(false);
        pRow->AddItem(pName);

        ui::Label* pSize = new ui::Label(this);
        pSize->SetText(entry.isDir ? _T("—") : FormatSize(entry.size));
        pSize->SetAttribute(_T("font"), _T("system_12"));
        pSize->SetAttribute(_T("text_color"), kTextHint);
        pSize->SetAttribute(_T("text_align"), _T("right,vcenter"));
        pSize->SetAttribute(_T("width"), _T("90"));
        pSize->SetAttribute(_T("height"), _T("stretch"));
        pSize->SetMouseEnabled(false);
        pRow->AddItem(pSize);

        ui::Label* pTime = new ui::Label(this);
        pTime->SetText(entry.mtime);
        pTime->SetAttribute(_T("font"), _T("system_12"));
        pTime->SetAttribute(_T("text_color"), kTextHint);
        pTime->SetAttribute(_T("text_align"), _T("right,vcenter"));
        pTime->SetAttribute(_T("width"), _T("150"));
        pTime->SetAttribute(_T("margin"), _T("0,0,10,0"));
        pTime->SetAttribute(_T("height"), _T("stretch"));
        pTime->SetMouseEnabled(false);
        pRow->AddItem(pTime);

        m_pFileList->AddItem(pRow);
    }

    if (m_pStatusLabel != nullptr) {
        // Finder-style status bar: item count.
        DString status = ui::StringUtil::Printf(
            _T("%d 个项目"), dirCount + fileCount);
        m_pStatusLabel->SetText(status);
        m_pStatusLabel->SetAttribute(_T("text_color"), kTextHint);
    }
}

void FilesForm::UpdatePathLabel()
{
    if (m_pPathLabel == nullptr) {
        return;
    }
    // Finder shows the current folder name in the toolbar/title, not the
    // full POSIX path. "/" gets a friendly name.
    DString name = m_curDir;
    if (name == _T("/")) {
        name = _T("PolluxOS");
    } else {
        size_t pos = name.find_last_of(_T('/'));
        if (pos != DString::npos && pos + 1 < name.size()) {
            name = name.substr(pos + 1);
        }
    }
    m_pPathLabel->SetText(name);
}

void FilesForm::ShellQuote(DString& arg)
{
    DString quoted = _T("'");
    for (DString::value_type ch : arg) {
        if (ch == _T('\'')) {
            quoted += _T("'\\''");
        } else {
            quoted.push_back(ch);
        }
    }
    quoted += _T("'");
    arg = quoted;
}

void FilesForm::OpenEntry(const Entry& entry)
{
    DString full = m_curDir;
    if (!full.empty() && full.back() != _T('/')) {
        full += _T('/');
    }
    full += entry.name;

    if (entry.isDir) {
        Navigate(full);
        return;
    }

    // Executables run directly; everything else opens in wayst+vim.
    struct stat st;
    if (stat(full.c_str(), &st) == 0 &&
        S_ISREG(st.st_mode) && (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0 &&
        !IsTextExtension(entry.name)) {
        DString cmd = full;
        ShellQuote(cmd);
        LaunchCommand(cmd);
        return;
    }

    DString quoted = full;
    ShellQuote(quoted);
    LaunchCommand(DString(_T("wayst -e vim ")) + quoted);
}

void FilesForm::LaunchCommand(const DString& cmdline)
{
    printf("[polluxdesk-files] launch: %s\n", cmdline.c_str());
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        perror("[polluxdesk-files] fork");
        return;
    }
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmdline.c_str(), static_cast<char*>(nullptr));
        perror("[polluxdesk-files] execl");
        _exit(127);
    }
}
