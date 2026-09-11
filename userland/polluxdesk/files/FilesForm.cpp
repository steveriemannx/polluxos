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
const DString kBarBg        = DUI_T("#F2FFFFFF");
const DString kBarBorder    = DUI_T("#33000000");
const DString kTransparent  = DUI_T("#00000000");
const DString kListBg       = DUI_T("#F9FFFFFF");
const DString kRowHot       = DUI_T("#220A84FF");
const DString kTextDark     = DUI_T("#FF1D1D1F");
const DString kTextBody     = DUI_T("#FF3A3A3C");
const DString kTextHint     = DUI_T("#FF8E8E93");
const DString kAccent       = DUI_T("#FF0A84FF");
const DString kFolderColor  = DUI_T("#FF4C9FDB");

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
    return DUI_T("");
}

DString FilesForm::GetSkinFile()
{
    // Pure code mode: no layout XML is loaded
    return DUI_T("");
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
                                            : DString(DUI_T("/")));
}

void FilesForm::BuildUi()
{
    ui::VBox* pRoot = new ui::VBox(this);
    pRoot->SetBkColor(kTransparent);
    pRoot->SetBorderColor(kTransparent);
    pRoot->SetAttribute(DUI_T("border_size"), DUI_T("0"));
    pRoot->SetAttribute(DUI_T("padding"), DUI_T("0,0,0,0"));

    BuildToolbar(pRoot);
    BuildFileList(pRoot);
    BuildStatusBar(pRoot);

    AttachBox(pRoot);
}

static void MakeToolButton(ui::Button* pBtn, const DString& text)
{
    pBtn->SetText(text);
    pBtn->SetAttribute(DUI_T("font"), DUI_T("system_14"));
    pBtn->SetAttribute(DUI_T("text_color"), kTextDark);
    pBtn->SetAttribute(DUI_T("text_align"), DUI_T("hcenter,vcenter"));
    pBtn->SetAttribute(DUI_T("height"), DUI_T("30"));
    pBtn->SetAttribute(DUI_T("width"), DUI_T("auto"));
    pBtn->SetAttribute(DUI_T("margin"), DUI_T("4,3,4,3"));
    pBtn->SetAttribute(DUI_T("text_padding"), DUI_T("10,0,10,0"));
    pBtn->SetStateColor(ui::kControlStateNormal, kTransparent);
    pBtn->SetStateColor(ui::kControlStateHot, kRowHot);
    pBtn->SetStateColorRound(ui::kControlStateNormal, ui::UiSize(6, 6), false);
    pBtn->SetStateColorRound(ui::kControlStateHot, ui::UiSize(6, 6), false);
    pBtn->SetAttribute(DUI_T("cursor_type"), DUI_T("hand"));
}

void FilesForm::BuildToolbar(ui::VBox* pRoot)
{
    // Finder-style toolbar: just the back / forward chevrons and the current
    // folder name. (macOS keeps the toolbar minimal; no terminal/refresh.)
    ui::HBox* pBar = new ui::HBox(this);
    pBar->SetAttribute(DUI_T("height"), DUI_T("40"));
    pBar->SetBkColor(kBarBg);
    pBar->SetBorderColor(kBarBorder);
    pBar->SetAttribute(DUI_T("bottom_border_size"), DUI_T("1"));
    pBar->SetAttribute(DUI_T("padding"), DUI_T("8,0,8,0"));
    pRoot->AddItem(pBar);

    ui::Button* pBack = new ui::Button(this);
    MakeToolButton(pBack, DUI_T("‹"));
    pBack->AttachClick([this](const ui::EventArgs& /*args*/) {
        NavigateBack();
        return true;
    });
    pBar->AddItem(pBack);

    ui::Button* pForward = new ui::Button(this);
    MakeToolButton(pForward, DUI_T("›"));
    pForward->AttachClick([this](const ui::EventArgs& /*args*/) {
        NavigateForward();
        return true;
    });
    pBar->AddItem(pForward);

    // Current folder name, Finder style (bold, centered in the toolbar).
    m_pPathLabel = new ui::Label(this);
    m_pPathLabel->SetAttribute(DUI_T("font"), DUI_T("system_bold_16"));
    m_pPathLabel->SetAttribute(DUI_T("text_color"), kTextDark);
    m_pPathLabel->SetAttribute(DUI_T("text_align"), DUI_T("hcenter,vcenter"));
    m_pPathLabel->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    m_pPathLabel->SetText(DUI_T(""));
    m_pPathLabel->SetMouseEnabled(false);
    pBar->AddItem(m_pPathLabel);
}

// Small macOS-style folder icon: a blue gradient rounded square with a
// darker "tab" strip at the top.
static ui::VBox* MakeFolderIcon(ui::Window* pWindow)
{
    ui::VBox* pIcon = new ui::VBox(pWindow);
    pIcon->SetAttribute(DUI_T("width"), DUI_T("26"));
    pIcon->SetAttribute(DUI_T("height"), DUI_T("20"));
    pIcon->SetBkColor(DUI_T("#FF63B0ED"));
    pIcon->SetBkColor2(DUI_T("#FF3E8ECA"));
    pIcon->SetBkColor2Direction(DUI_T("1"));   // left -> right gradient
    pIcon->SetStateColorRound(ui::kControlStateNormal, ui::UiSize(5, 5), false);
    pIcon->SetAttribute(DUI_T("border_round"), DUI_T("5,5"));
    pIcon->SetMouseEnabled(false);

    // The folder tab: a darker rounded strip peeking from the top-left.
    ui::Control* pTab = new ui::Control(pWindow);
    pTab->SetAttribute(DUI_T("width"), DUI_T("12"));
    pTab->SetAttribute(DUI_T("height"), DUI_T("4"));
    pTab->SetAttribute(DUI_T("margin"), DUI_T("-1,-2,0,0"));
    pTab->SetAttribute(DUI_T("halign"), DUI_T("left"));
    pTab->SetBkColor(DUI_T("#FF2F7AB8"));
    pTab->SetMouseEnabled(false);
    pIcon->AddItem(pTab);
    return pIcon;
}

// Small document icon: a white page with faint text lines.
static ui::VBox* MakeFileIcon(ui::Window* pWindow)
{
    ui::VBox* pIcon = new ui::VBox(pWindow);
    pIcon->SetAttribute(DUI_T("width"), DUI_T("18"));
    pIcon->SetAttribute(DUI_T("height"), DUI_T("20"));
    pIcon->SetBkColor(DUI_T("#FFFFFFFF"));
    pIcon->SetBorderColor(DUI_T("#FFC9C9CE"));
    pIcon->SetAttribute(DUI_T("border_size"), DUI_T("1"));
    pIcon->SetStateColorRound(ui::kControlStateNormal, ui::UiSize(3, 3), false);
    pIcon->SetAttribute(DUI_T("border_round"), DUI_T("3,3"));
    pIcon->SetAttribute(DUI_T("child_align"), DUI_T("hcenter,vcenter"));
    pIcon->SetMouseEnabled(false);
    for (int i = 0; i < 3; ++i) {
        if (i > 0) {
            ui::Control* pGap = new ui::Control(pWindow);
            pGap->SetAttribute(DUI_T("height"), DUI_T("2"));
            pGap->SetMouseEnabled(false);
            pIcon->AddItem(pGap);
        }
        ui::Control* pLine = new ui::Control(pWindow);
        pLine->SetAttribute(DUI_T("height"), DUI_T("1"));
        pLine->SetAttribute(DUI_T("width"), DUI_T("10"));
        pLine->SetBkColor(DUI_T("#FFC7C7CC"));
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
    m_pFileList->SetAttribute(DUI_T("height"), DUI_T("stretch"));
    m_pFileList->SetBkColor(kListBg);
    m_pFileList->SetAttribute(DUI_T("padding"), DUI_T("6,6,6,6"));
    m_pFileList->SetAttribute(DUI_T("vscrollbar"), DUI_T("true"));
    pRoot->AddItem(m_pFileList);
}

void FilesForm::BuildStatusBar(ui::VBox* pRoot)
{
    ui::HBox* pBar = new ui::HBox(this);
    pBar->SetAttribute(DUI_T("height"), DUI_T("26"));
    pBar->SetBkColor(kBarBg);
    pBar->SetBorderColor(kBarBorder);
    pBar->SetAttribute(DUI_T("top_border_size"), DUI_T("1"));
    pBar->SetAttribute(DUI_T("padding"), DUI_T("10,0,10,0"));
    pRoot->AddItem(pBar);

    m_pStatusLabel = new ui::Label(this);
    m_pStatusLabel->SetAttribute(DUI_T("font"), DUI_T("system_12"));
    m_pStatusLabel->SetAttribute(DUI_T("text_color"), kTextHint);
    m_pStatusLabel->SetAttribute(DUI_T("text_align"), DUI_T("left,vcenter"));
    m_pStatusLabel->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    m_pStatusLabel->SetText(DUI_T(""));
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
        if (!full.empty() && full.back() != DUI_T('/')) {
            full += DUI_T('/');
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
            m_pStatusLabel->SetText(DUI_T("无法打开目录：") + path);
            m_pStatusLabel->SetAttribute(DUI_T("text_color"), DUI_T("#FFFF453A"));
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
        pRow->SetAttribute(DUI_T("height"), DUI_T("30"));
        pRow->SetAttribute(DUI_T("width"), DUI_T("stretch"));
        pRow->SetAttribute(DUI_T("padding"), DUI_T("6,0,6,0"));
        pRow->SetStateColor(ui::kControlStateNormal, kTransparent);
        pRow->SetStateColor(ui::kControlStateHot, kRowHot);
        pRow->SetStateColorRound(ui::kControlStateNormal, ui::UiSize(6, 6), false);
        pRow->SetStateColorRound(ui::kControlStateHot, ui::UiSize(6, 6), false);
        pRow->SetAttribute(DUI_T("cursor_type"), DUI_T("arrow"));

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
        pIconCell->SetAttribute(DUI_T("width"), DUI_T("36"));
        pIconCell->SetAttribute(DUI_T("height"), DUI_T("stretch"));
        pIconCell->SetAttribute(DUI_T("child_align"), DUI_T("hcenter,vcenter"));
        pIconCell->SetMouseEnabled(false);
        pRow->AddItem(pIconCell);
        if (entry.isDir) {
            pIconCell->AddItem(MakeFolderIcon(this));
        } else {
            pIconCell->AddItem(MakeFileIcon(this));
        }

        ui::Label* pName = new ui::Label(this);
        pName->SetText(entry.name);
        pName->SetAttribute(DUI_T("font"), DUI_T("system_14"));
        pName->SetAttribute(DUI_T("text_color"),
                            entry.isDir ? kAccent : kTextDark);
        pName->SetAttribute(DUI_T("text_align"), DUI_T("left,vcenter"));
        pName->SetAttribute(DUI_T("width"), DUI_T("stretch"));
        pName->SetAttribute(DUI_T("height"), DUI_T("stretch"));
        pName->SetMouseEnabled(false);
        pRow->AddItem(pName);

        ui::Label* pSize = new ui::Label(this);
        pSize->SetText(entry.isDir ? DUI_T("—") : FormatSize(entry.size));
        pSize->SetAttribute(DUI_T("font"), DUI_T("system_12"));
        pSize->SetAttribute(DUI_T("text_color"), kTextHint);
        pSize->SetAttribute(DUI_T("text_align"), DUI_T("right,vcenter"));
        pSize->SetAttribute(DUI_T("width"), DUI_T("90"));
        pSize->SetAttribute(DUI_T("height"), DUI_T("stretch"));
        pSize->SetMouseEnabled(false);
        pRow->AddItem(pSize);

        ui::Label* pTime = new ui::Label(this);
        pTime->SetText(entry.mtime);
        pTime->SetAttribute(DUI_T("font"), DUI_T("system_12"));
        pTime->SetAttribute(DUI_T("text_color"), kTextHint);
        pTime->SetAttribute(DUI_T("text_align"), DUI_T("right,vcenter"));
        pTime->SetAttribute(DUI_T("width"), DUI_T("150"));
        pTime->SetAttribute(DUI_T("margin"), DUI_T("0,0,10,0"));
        pTime->SetAttribute(DUI_T("height"), DUI_T("stretch"));
        pTime->SetMouseEnabled(false);
        pRow->AddItem(pTime);

        m_pFileList->AddItem(pRow);
    }

    if (m_pStatusLabel != nullptr) {
        // Finder-style status bar: item count.
        DString status = ui::StringUtil::Printf(
            DUI_T("%d 个项目"), dirCount + fileCount);
        m_pStatusLabel->SetText(status);
        m_pStatusLabel->SetAttribute(DUI_T("text_color"), kTextHint);
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
    if (name == DUI_T("/")) {
        name = DUI_T("PolluxOS");
    } else {
        size_t pos = name.find_last_of(DUI_T('/'));
        if (pos != DString::npos && pos + 1 < name.size()) {
            name = name.substr(pos + 1);
        }
    }
    m_pPathLabel->SetText(name);
}

void FilesForm::ShellQuote(DString& arg)
{
    DString quoted = DUI_T("'");
    for (DString::value_type ch : arg) {
        if (ch == DUI_T('\'')) {
            quoted += DUI_T("'\\''");
        } else {
            quoted.push_back(ch);
        }
    }
    quoted += DUI_T("'");
    arg = quoted;
}

void FilesForm::OpenEntry(const Entry& entry)
{
    DString full = m_curDir;
    if (!full.empty() && full.back() != DUI_T('/')) {
        full += DUI_T('/');
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
    LaunchCommand(DString(DUI_T("wayst -e vim ")) + quoted);
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
