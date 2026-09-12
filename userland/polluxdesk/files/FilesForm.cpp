#include "FilesForm.h"

#include <ctime>
#include <cstdio>
#include <string>
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
const DString kSidebarBg    = DUI_T("#E8F2F3F7");
const DString kListBg       = DUI_T("#F9FFFFFF");
const DString kRowHot       = DUI_T("#220A84FF");
const DString kRowSelected  = DUI_T("#330A84FF");
const DString kTextDark     = DUI_T("#FF1D1D1F");
const DString kTextBody     = DUI_T("#FF3A3A3C");
const DString kTextHint     = DUI_T("#FF8E8E93");
const DString kTextOnAccent = DUI_T("#FFFFFFFF");
const DString kAccent       = DUI_T("#FF0A84FF");
const DString kHairline     = DUI_T("#22000000");
const DString kFolderColor   = DUI_T("#FF4C9FDB");

const int kRowHeight  = 30;
const int kCrumbW     = 72;   // one path segment in the breadcrumb
const int kIconTileW  = 100;
const int kIconTileH  = 96;

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

// dui takes a corner radius through two independent APIs that must agree, so
// everything rounded goes through here.
void SetRadius(ui::Control* pControl, int radius, bool interactive)
{
    const ui::UiSize size(radius, radius);
    pControl->SetStateColorRound(ui::kControlStateNormal, size, false);
    if (interactive) {
        pControl->SetStateColorRound(ui::kControlStateHot, size, false);
        pControl->SetStateColorRound(ui::kControlStatePushed, size, false);
    }
    pControl->SetAttribute(DUI_T("border_round"),
        ui::StringUtil::Printf(DUI_T("%d,%d"), radius, radius));
}

DString Num(int value)
{
    return ui::StringUtil::Printf(DUI_T("%d"), value);
}

// A hairline used between the panes.
ui::Control* MakeHairline(ui::Window* pWindow, bool vertical)
{
    ui::Control* pLine = new ui::Control(pWindow);
    pLine->SetAttribute(DUI_T("width"), vertical ? DUI_T("1") : DUI_T("stretch"));
    pLine->SetAttribute(DUI_T("height"), vertical ? DUI_T("stretch") : DUI_T("1"));
    pLine->SetBkColor(kHairline);
    pLine->SetMouseEnabled(false);
    return pLine;
}

// A small round-cornered glyph box: the folder tab and the document fold are
// the only detail that survives at this size.
ui::VBox* MakeFolderIcon(ui::Window* pWindow, int size)
{
    ui::VBox* pIcon = new ui::VBox(pWindow);
    pIcon->SetAttribute(DUI_T("width"), Num(size * 13 / 10));
    pIcon->SetAttribute(DUI_T("height"), Num(size));
    pIcon->SetBkColor(kFolderColor);
    pIcon->SetBkColor2(DUI_T("#FF3E8ECA"));
    pIcon->SetBkColor2Direction(DUI_T("1"));
    SetRadius(pIcon, std::max(3, size / 5), false);
    pIcon->SetMouseEnabled(false);

    ui::Control* pTab = new ui::Control(pWindow);
    pTab->SetAttribute(DUI_T("width"), Num(size * 3 / 5));
    pTab->SetAttribute(DUI_T("height"), Num(size / 5));
    pTab->SetAttribute(DUI_T("margin"), DUI_T("-1,-2,0,0"));
    pTab->SetAttribute(DUI_T("halign"), DUI_T("left"));
    pTab->SetBkColor(DUI_T("#FF2F7AB8"));
    pTab->SetMouseEnabled(false);
    pIcon->AddItem(pTab);
    return pIcon;
}

ui::VBox* MakeFileIcon(ui::Window* pWindow, int size)
{
    ui::VBox* pIcon = new ui::VBox(pWindow);
    pIcon->SetAttribute(DUI_T("width"), Num(size * 9 / 10));
    pIcon->SetAttribute(DUI_T("height"), Num(size));
    pIcon->SetBkColor(DUI_T("#FFFFFFFF"));
    pIcon->SetBorderColor(DUI_T("#FFC9C9CE"));
    pIcon->SetAttribute(DUI_T("border_size"), DUI_T("1"));
    SetRadius(pIcon, std::max(2, size / 7), false);
    pIcon->SetAttribute(DUI_T("child_align"), DUI_T("vcenter"));
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
        pLine->SetAttribute(DUI_T("width"), DUI_T("stretch"));
        pLine->SetAttribute(DUI_T("margin"), DUI_T("2,0,2,0"));
        pLine->SetAttribute(DUI_T("halign"), DUI_T("center"));
        pLine->SetBkColor(DUI_T("#FFC7C7CC"));
        pLine->SetMouseEnabled(false);
        pIcon->AddItem(pLine);
    }
    return pIcon;
}

// A flat toolbar button with a glyph, as the browser's own toolbar uses.
ui::Button* MakeToolButton(ui::Window* pWindow, const DString& glyph,
                           const DString& tip, int width)
{
    ui::Button* pButton = new ui::Button(pWindow);
    pButton->SetText(glyph);
    pButton->SetAttribute(DUI_T("font"), DUI_T("system_16"));
    pButton->SetAttribute(DUI_T("width"), Num(width));
    pButton->SetAttribute(DUI_T("height"), DUI_T("30"));
    pButton->SetAttribute(DUI_T("margin"), DUI_T("0,0,4,0"));
    pButton->SetAttribute(DUI_T("text_align"), DUI_T("hcenter,vcenter"));
    pButton->SetAttribute(DUI_T("cursor_type"), DUI_T("hand"));
    pButton->SetStateColor(ui::kControlStateNormal, kTransparent);
    pButton->SetStateColor(ui::kControlStateHot, kRowHot);
    pButton->SetStateColor(ui::kControlStatePushed, kRowSelected);
    pButton->SetStateTextColor(ui::kControlStateNormal, kTextBody);
    pButton->SetStateTextColor(ui::kControlStateDisabled, kTextHint);
    SetRadius(pButton, 6, true);
    pButton->SetToolTipText(tip);
    return pButton;
}

// One line of text in the status bar.
ui::Label* MakeStatusText(ui::Window* pWindow, const DString& text,
                          const DString& colour, const DString& align)
{
    ui::Label* pLabel = new ui::Label(pWindow);
    pLabel->SetText(text);
    pLabel->SetAttribute(DUI_T("font"), DUI_T("system_12"));
    pLabel->SetStateTextColor(ui::kControlStateNormal, colour);
    pLabel->SetAttribute(DUI_T("text_align"), align);
    pLabel->SetAttribute(DUI_T("height"), DUI_T("stretch"));
    pLabel->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    pLabel->SetMouseEnabled(false);
    return pLabel;
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
    return DUI_T("polluxdesk");
}

DString FilesForm::GetSkinFile()
{
    return DUI_T("files.xml");
}

void FilesForm::GetCreateWindowAttributes(ui::WindowCreateAttributes& attrs)
{
    BaseClass::GetCreateWindowAttributes(attrs);
}

void FilesForm::OnInitWindow()
{
    SetShadowAttached(false);
    BuildUi();

    BaseClass::OnInitWindow();

    // Open where we were asked to, falling back rather than leaving the
    // window empty: the dock asks for the trash, which does not exist until
    // something has been thrown away.
    if (m_startDir.empty() || !Navigate(m_startDir)) {
        const char* home = std::getenv("HOME");
        if (home == nullptr || !Navigate(DString(home))) {
            Navigate(DUI_T("/"));
        }
    }
}

void FilesForm::BuildUi()
{
    ui::VBox* pRoot = new ui::VBox(this);
    pRoot->SetBkColor(kListBg);
    pRoot->SetBorderColor(kTransparent);
    pRoot->SetAttribute(DUI_T("border_size"), DUI_T("0"));
    pRoot->SetAttribute(DUI_T("padding"), DUI_T("0,0,0,0"));

    BuildToolbar(pRoot);
    BuildBody(pRoot);
    BuildStatusBar(pRoot);
    BuildContextMenu();
    pRoot->AddItem(m_pContextMenu);

    AttachBox(pRoot);
}

void FilesForm::BuildToolbar(ui::VBox* pRoot)
{
    ui::VBox* pToolbar = new ui::VBox(this);
    pToolbar->SetAttribute(DUI_T("height"), DUI_T("auto"));
    pToolbar->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    pToolbar->SetAttribute(DUI_T("padding"), DUI_T("10,8,10,8"));
    pToolbar->SetBkColor(kBarBg);

    // Row one is the buttons. They get a row to themselves rather than
    // sharing with the path: a row that holds both a stretch item and several
    // fixed ones lays out unpredictably, and whatever lost the width was
    // simply not drawn.
    ui::HBox* pRow = new ui::HBox(this);
    pRow->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    pRow->SetAttribute(DUI_T("height"), DUI_T("30"));

    m_pBackButton = MakeToolButton(this, DUI_T("‹"), DUI_T("后退"), 34);
    m_pBackButton->AttachClick([this](const ui::EventArgs&) {
        NavigateBack();
        return true;
    });
    pRow->AddItem(m_pBackButton);

    m_pForwardButton = MakeToolButton(this, DUI_T("›"), DUI_T("前进"), 34);
    m_pForwardButton->AttachClick([this](const ui::EventArgs&) {
        NavigateForward();
        return true;
    });
    pRow->AddItem(m_pForwardButton);

    ui::Button* pUp = MakeToolButton(this, DUI_T("↑"), DUI_T("上一级"), 34);
    pUp->AttachClick([this](const ui::EventArgs&) {
        NavigateUp();
        return true;
    });
    pRow->AddItem(pUp);

    ui::Control* pSpacer = new ui::Control(this);
    pSpacer->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    pSpacer->SetMouseEnabled(false);
    pRow->AddItem(pSpacer);

    // Text rather than symbols: the embedded font has the arrows but not most
    // of the geometric shapes, which come out as empty boxes.
    ui::Button* pNewFolder = MakeToolButton(this, DUI_T("新建"), DUI_T("新建文件夹"), 52);
    pNewFolder->AttachClick([this](const ui::EventArgs&) {
        NewFolder();
        return true;
    });
    pRow->AddItem(pNewFolder);

    ui::Button* pView = MakeToolButton(this, DUI_T("视图"), DUI_T("切换列表 / 图标"), 52);
    pView->AttachClick([this](const ui::EventArgs&) {
        SetIconView(!m_iconView);
        return true;
    });
    pRow->AddItem(pView);

    ui::Button* pRefresh = MakeToolButton(this, DUI_T("刷新"), DUI_T("刷新"), 52);
    pRefresh->AttachClick([this](const ui::EventArgs&) {
        Refresh();
        return true;
    });
    pRow->AddItem(pRefresh);
    pToolbar->AddItem(pRow);

    // Row two is the path. Its segments are built once here at a fixed width
    // each: dui lays a control out during the layout pass and does not re-run
    // it for a box that merely gained children, so segments created on the
    // way into a directory would exist but never get a size. Navigation only
    // rewrites their text.
    m_pBreadcrumb = new ui::HBox(this);
    m_pBreadcrumb->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    m_pBreadcrumb->SetAttribute(DUI_T("height"), DUI_T("28"));
    m_pBreadcrumb->SetAttribute(DUI_T("margin"), DUI_T("0,6,0,0"));
    for (int i = 0; i < kCrumbSlots; ++i) {
        if (i > 0) {
            ui::Label* pSep = new ui::Label(this);
            pSep->SetText(DUI_T("›"));
            pSep->SetAttribute(DUI_T("font"), DUI_T("system_13"));
            pSep->SetStateTextColor(ui::kControlStateNormal, kTextHint);
            pSep->SetAttribute(DUI_T("width"), DUI_T("14"));
            pSep->SetAttribute(DUI_T("height"), DUI_T("stretch"));
            pSep->SetAttribute(DUI_T("text_align"), DUI_T("hcenter,vcenter"));
            pSep->SetMouseEnabled(false);
            m_pBreadcrumb->AddItem(pSep);
        }
        ui::Button* pCrumb = new ui::Button(this);
        pCrumb->SetAttribute(DUI_T("font"), DUI_T("system_13"));
        pCrumb->SetAttribute(DUI_T("text_align"), DUI_T("hcenter,vcenter"));
        pCrumb->SetAttribute(DUI_T("text_padding"), DUI_T("4,0,4,0"));
        pCrumb->SetAttribute(DUI_T("height"), DUI_T("stretch"));
        pCrumb->SetAttribute(DUI_T("width"), Num(kCrumbW));
        pCrumb->SetAttribute(DUI_T("cursor_type"), DUI_T("hand"));
        pCrumb->SetStateColor(ui::kControlStateNormal, kTransparent);
        pCrumb->SetStateColor(ui::kControlStateHot, kRowHot);
        SetRadius(pCrumb, 5, true);
        const int slot = i;
        pCrumb->AttachClick([this, slot](const ui::EventArgs&) {
            if (!m_crumbTargets[slot].empty()) {
                Navigate(m_crumbTargets[slot]);
            }
            return true;
        });
        m_pBreadcrumb->AddItem(pCrumb);
        m_pCrumbButtons[i] = pCrumb;
    }
    pToolbar->AddItem(m_pBreadcrumb);

    pRoot->AddItem(pToolbar);
    pRoot->AddItem(MakeHairline(this, false));
}

void FilesForm::BuildBody(ui::VBox* pRoot)
{
    ui::HBox* pBody = new ui::HBox(this);
    pBody->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    pBody->SetAttribute(DUI_T("height"), DUI_T("stretch"));
    BuildSidebar(pBody);
    pBody->AddItem(MakeHairline(this, true));
    BuildContent(pBody);
    pRoot->AddItem(pBody);
}

void FilesForm::BuildSidebar(ui::HBox* pParent)
{
    m_pSidebar = new ui::VBox(this);
    m_pSidebar->SetAttribute(DUI_T("width"), DUI_T("180"));
    m_pSidebar->SetAttribute(DUI_T("height"), DUI_T("stretch"));
    m_pSidebar->SetAttribute(DUI_T("padding"), DUI_T("8,10,8,10"));
    m_pSidebar->SetBkColor(kSidebarBg);

    ui::Label* pTitle = new ui::Label(this);
    pTitle->SetText(DUI_T("位置"));
    pTitle->SetAttribute(DUI_T("font"), DUI_T("system_12"));
    pTitle->SetStateTextColor(ui::kControlStateNormal, kTextHint);
    pTitle->SetAttribute(DUI_T("text_align"), DUI_T("left,vcenter"));
    pTitle->SetAttribute(DUI_T("height"), DUI_T("24"));
    pTitle->SetMouseEnabled(false);
    m_pSidebar->AddItem(pTitle);

    const char* home = std::getenv("HOME");
    const DString homeDir(home != nullptr ? home : "/");

    std::vector<Place> places;
    places.push_back({ "主目录", homeDir, DUI_T("🏠") });
    struct Shortcut { const char* label; const char* sub; };
    const Shortcut shortcuts[] = {
        { "桌面", "Desktop" }, { "文档", "Documents" }, { "下载", "Downloads" },
        { "音乐", "Music" }, { "图片", "Pictures" }, { "视频", "Videos" },
    };
    for (const Shortcut& shortcut : shortcuts) {
        const DString path = homeDir + DUI_T("/") + DString(shortcut.sub);
        struct stat st;
        if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            places.push_back({ shortcut.label, path, DUI_T("") });
        }
    }
    places.push_back({ "废纸篓", homeDir + DUI_T("/.Trash"), DUI_T("") });
    places.push_back({ "文件系统", DUI_T("/"), DUI_T("") });

    for (const Place& place : places) {
        ui::Button* pItem = new ui::Button(this);
        pItem->SetText(DString(place.label));
        pItem->SetAttribute(DUI_T("font"), DUI_T("system_14"));
        pItem->SetAttribute(DUI_T("text_align"), DUI_T("left,vcenter"));
        pItem->SetAttribute(DUI_T("text_padding"), DUI_T("10,0,10,0"));
        pItem->SetAttribute(DUI_T("width"), DUI_T("stretch"));
        pItem->SetAttribute(DUI_T("height"), DUI_T("30"));
        pItem->SetAttribute(DUI_T("margin"), DUI_T("0,1,0,1"));
        pItem->SetAttribute(DUI_T("cursor_type"), DUI_T("hand"));
        pItem->SetStateColor(ui::kControlStateNormal, kTransparent);
        pItem->SetStateColor(ui::kControlStateHot, kRowHot);
        pItem->SetStateTextColor(ui::kControlStateNormal, kTextBody);
        SetRadius(pItem, 6, true);
        const DString path = place.path;
        pItem->AttachClick([this, path](const ui::EventArgs&) {
            Navigate(path);
            return true;
        });
        m_pSidebar->AddItem(pItem);
        m_pPlaceButtons.push_back(pItem);
        if (place.path == m_curDir) {
            pItem->SetStateColor(ui::kControlStateNormal, kRowSelected);
        }
    }

    // The sidebar's own record of where each button points, so the highlight
    // can follow navigation. Stored on the button's tooltip, which is unused
    // here and survives without another parallel array to keep in step.
    pParent->AddItem(m_pSidebar);
}

void FilesForm::BuildContent(ui::HBox* pParent)
{
    ui::VBox* pContent = new ui::VBox(this);
    pContent->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    pContent->SetAttribute(DUI_T("height"), DUI_T("stretch"));
    pContent->SetAttribute(DUI_T("padding"), DUI_T("0,0,0,0"));

    // Column headings. Clicking one sorts by it and clicks again reverse it,
    // which is the one thing every file browser's list view shares.
    m_pHeaderRow = new ui::HBox(this);
    m_pHeaderRow->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    m_pHeaderRow->SetAttribute(DUI_T("height"), DUI_T("26"));
    m_pHeaderRow->SetAttribute(DUI_T("padding"), DUI_T("8,0,8,0"));
    m_pHeaderRow->SetBkColor(kBarBg);

    const char* kColumnNames[] = { "名称", "大小", "修改日期" };
    const int kColumnWidths[] = { 0, 110, 160 };   // 0 = stretch
    for (int i = 0; i < 3; ++i) {
        ui::Button* pColumn = new ui::Button(this);
        pColumn->SetAttribute(DUI_T("font"), DUI_T("system_12"));
        pColumn->SetStateTextColor(ui::kControlStateNormal, kTextHint);
        pColumn->SetAttribute(DUI_T("text_align"), DUI_T("left,vcenter"));
        pColumn->SetAttribute(DUI_T("text_padding"), DUI_T("6,0,6,0"));
        pColumn->SetAttribute(DUI_T("height"), DUI_T("stretch"));
        pColumn->SetAttribute(DUI_T("width"),
            kColumnWidths[i] == 0 ? DUI_T("stretch") : Num(kColumnWidths[i]));
        pColumn->SetAttribute(DUI_T("cursor_type"), DUI_T("hand"));
        pColumn->SetStateColor(ui::kControlStateNormal, kTransparent);
        pColumn->SetStateColor(ui::kControlStateHot, kRowHot);
        pColumn->AttachClick([this, i](const ui::EventArgs&) {
            if (m_sortColumn == i) {
                m_sortAscending = !m_sortAscending;
            } else {
                m_sortColumn = i;
                m_sortAscending = true;
            }
            ReloadList();
            return true;
        });
        pColumn->SetText(DString(kColumnNames[i]));
        m_pHeaderRow->AddItem(pColumn);
        m_pColumnButtons[i] = pColumn;
    }
    pContent->AddItem(m_pHeaderRow);

    m_pFileList = new ui::VScrollBox(this);
    m_pFileList->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    m_pFileList->SetAttribute(DUI_T("height"), DUI_T("stretch"));
    m_pFileList->SetBkColor(kListBg);
    m_pFileList->SetBorderColor(kTransparent);
    pContent->AddItem(m_pFileList);

    pParent->AddItem(pContent);
}

void FilesForm::BuildStatusBar(ui::VBox* pRoot)
{
    ui::HBox* pStatus = new ui::HBox(this);
    pStatus->SetAttribute(DUI_T("height"), DUI_T("30"));
    pStatus->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    pStatus->SetAttribute(DUI_T("padding"), DUI_T("12,0,12,0"));
    pStatus->SetBkColor(kBarBg);
    pStatus->AddItem(MakeHairline(this, false));

    ui::HBox* pRow = new ui::HBox(this);
    pRow->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    pRow->SetAttribute(DUI_T("height"), DUI_T("stretch"));
    pRow->AddItem(nullptr);
    pRow->RemoveAllItems();

    m_pStatusLabel = MakeStatusText(this, DUI_T(""), kTextHint, DUI_T("left,vcenter"));
    pRow->AddItem(m_pStatusLabel);
    m_pSelectionLabel = MakeStatusText(this, DUI_T(""), kTextHint, DUI_T("right,vcenter"));
    pRow->AddItem(m_pSelectionLabel);

    pStatus->AddItem(pRow);
    pRoot->AddItem(pStatus);
}

void FilesForm::BuildContextMenu()
{
    m_pContextMenu = new ui::VBox(this);
    m_pContextMenu->SetFloat(true);
    m_pContextMenu->SetKeepFloatPos(true);
    m_pContextMenu->SetBkColor(kBarBg);
    m_pContextMenu->SetBorderColor(kBarBorder);
    m_pContextMenu->SetAttribute(DUI_T("border_size"), DUI_T("1"));
    m_pContextMenu->SetAttribute(DUI_T("width"), DUI_T("180"));
    m_pContextMenu->SetAttribute(DUI_T("padding"), DUI_T("6,6,6,6"));
    SetRadius(m_pContextMenu, 9, false);
    m_pContextMenu->SetPaintOrder(200);
    m_pContextMenu->SetVisible(false);
}

bool FilesForm::Navigate(const DString& path)
{
    std::vector<Entry> entries;
    if (!ListDirectory(path, entries)) {
        if (m_pStatusLabel != nullptr) {
            m_pStatusLabel->SetText(DUI_T("无法打开目录：") + path);
        }
        return false;
    }
    if (!m_curDir.empty()) {
        m_history.push_back(m_curDir);
    }
    m_forward.clear();
    m_curDir = path;
    m_entries = std::move(entries);
    m_selected.clear();
    UpdateBreadcrumb();
    ReloadList();
    return true;
}

void FilesForm::NavigateBack()
{
    if (m_history.empty()) {
        return;
    }
    m_forward.push_back(m_curDir);
    const DString previous = m_history.back();
    m_history.pop_back();
    std::vector<Entry> entries;
    if (ListDirectory(previous, entries)) {
        m_curDir = previous;
        m_entries = std::move(entries);
        m_selected.clear();
        UpdateBreadcrumb();
        ReloadList();
    }
}

void FilesForm::NavigateForward()
{
    if (m_forward.empty()) {
        return;
    }
    const DString next = m_forward.back();
    std::vector<Entry> entries;
    if (ListDirectory(next, entries)) {
        m_forward.pop_back();
        if (!m_curDir.empty()) {
            m_history.push_back(m_curDir);
        }
        m_curDir = next;
        m_entries = std::move(entries);
        m_selected.clear();
        UpdateBreadcrumb();
        ReloadList();
    }
}

void FilesForm::NavigateUp()
{
    if (m_curDir.empty() || m_curDir == DUI_T("/")) {
        return;
    }
    DString parent = m_curDir;
    while (parent.size() > 1 && parent.back() == DUI_T('/')) {
        parent.pop_back();
    }
    const size_t slash = parent.find_last_of(DUI_T('/'));
    parent = (slash == DString::npos || slash == 0) ? DString(DUI_T("/"))
                                                    : parent.substr(0, slash);
    Navigate(parent);
}

void FilesForm::Refresh()
{
    std::vector<Entry> entries;
    if (ListDirectory(m_curDir, entries)) {
        m_entries = std::move(entries);
        ReloadList();
    }
}

void FilesForm::SetIconView(bool icons)
{
    m_iconView = icons;
    ReloadList();
}

bool FilesForm::IsSelected(size_t index) const
{
    return std::find(m_selected.begin(), m_selected.end(), index) != m_selected.end();
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
    return true;
}

void FilesForm::ReloadList()
{
    if (m_pFileList == nullptr) {
        return;
    }

    // Sorting lives here rather than in ListDirectory so the heading clicks
    // can pick the key. Directories lead in every column, as both Explorer
    // and Files do.
    std::sort(m_entries.begin(), m_entries.end(),
              [this](const Entry& a, const Entry& b) {
                  if (a.isDir != b.isDir) {
                      return a.isDir;
                  }
                  bool less;
                  switch (m_sortColumn) {
                  case 1:  less = a.size < b.size; break;
                  case 2:  less = a.mtime < b.mtime; break;
                  default: less = a.name < b.name; break;
                  }
                  return m_sortAscending ? less : !less;
              });

    // Mark the column the list is ordered by, the way both browsers do.
    const char* kColumnNames[] = { "名称", "大小", "修改日期" };
    for (int i = 0; i < 3; ++i) {
        if (m_pColumnButtons[i] == nullptr) {
            continue;
        }
        DString text(kColumnNames[i]);
        if (i == m_sortColumn) {
            text += m_sortAscending ? DUI_T(" ▲") : DUI_T(" ▼");
        }
        m_pColumnButtons[i]->SetText(text);
    }

    // The column headings only mean anything in the report view.
    if (m_pHeaderRow != nullptr) {
        m_pHeaderRow->SetVisible(!m_iconView);
    }

    m_pFileList->RemoveAllItems();
    m_selected.clear();

    if (m_iconView) {
        // A grid of tiles. The column count is taken from the pane's width
        // once, at build time: a resize re-lays the tiles but does not
        // re-flow them into a different number of columns.
        ui::UiRect client;
        GetClientRect(client);
        int width = client.Width() - 190;
        if (width < kIconTileW) {
            width = kIconTileW;
        }
        const int columns = std::max(1, width / kIconTileW);

        ui::HBox* pRow = nullptr;
        for (size_t i = 0; i < m_entries.size(); ++i) {
            if (i % columns == 0) {
                pRow = new ui::HBox(this);
                pRow->SetAttribute(DUI_T("width"), DUI_T("stretch"));
                pRow->SetAttribute(DUI_T("height"), Num(kIconTileH));
                m_pFileList->AddItem(pRow);
            }
            const Entry& entry = m_entries[i];

            ui::ButtonVBox* pTile = new ui::ButtonVBox(this);
            pTile->SetAttribute(DUI_T("width"), Num(kIconTileW));
            pTile->SetAttribute(DUI_T("height"), Num(kIconTileH));
            pTile->SetAttribute(DUI_T("margin"), DUI_T("4,4,4,4"));
            pTile->SetAttribute(DUI_T("cursor_type"), DUI_T("hand"));
            pTile->SetStateColor(ui::kControlStateNormal,
                                 IsSelected(i) ? kRowSelected : kTransparent);
            pTile->SetStateColor(ui::kControlStateHot, kRowHot);
            SetRadius(pTile, 8, true);

            ui::HBox* pIconCell = new ui::HBox(this);
            pIconCell->SetAttribute(DUI_T("width"), Num(kIconTileW - 8));
            pIconCell->SetAttribute(DUI_T("height"), DUI_T("56"));
            pIconCell->SetAttribute(DUI_T("halign"), DUI_T("center"));
            pIconCell->SetMouseEnabled(false);
            pIconCell->AddItem(entry.isDir ? MakeFolderIcon(this, 42)
                                           : MakeFileIcon(this, 42));
            pTile->AddItem(pIconCell);

            ui::Label* pName = new ui::Label(this);
            pName->SetText(entry.name);
            pName->SetAttribute(DUI_T("font"), DUI_T("system_12"));
            pName->SetStateTextColor(ui::kControlStateNormal,
                                     entry.isDir ? kAccent : kTextDark);
            pName->SetAttribute(DUI_T("text_align"), DUI_T("hcenter,vtop"));
            pName->SetAttribute(DUI_T("width"), Num(kIconTileW - 8));
            pName->SetAttribute(DUI_T("height"), DUI_T("34"));
            pName->SetAttribute(DUI_T("halign"), DUI_T("center"));
            pName->SetMouseEnabled(false);
            pTile->AddItem(pName);

            pTile->AttachClick([this, i](const ui::EventArgs& args) {
                SelectRow(i, args.modifierKey);
                return true;
            });
            pTile->AttachDoubleClick([this, i](const ui::EventArgs&) {
                OpenRow(i);
                return true;
            });
            pRow->AddItem(pTile);
        }
        UpdateStatus();
        return;
    }

    // Report view: icon, name, size, modified.
    for (size_t i = 0; i < m_entries.size(); ++i) {
        const Entry& entry = m_entries[i];
        ui::ButtonHBox* pRow = new ui::ButtonHBox(this);
        pRow->SetAttribute(DUI_T("height"), Num(kRowHeight));
        pRow->SetAttribute(DUI_T("width"), DUI_T("stretch"));
        pRow->SetAttribute(DUI_T("padding"), DUI_T("10,0,10,0"));
        pRow->SetStateColor(ui::kControlStateNormal,
                            IsSelected(i) ? kRowSelected : kTransparent);
        pRow->SetStateColor(ui::kControlStateHot, kRowHot);
        SetRadius(pRow, 6, true);
        pRow->SetAttribute(DUI_T("cursor_type"), DUI_T("hand"));

        ui::HBox* pIconCell = new ui::HBox(this);
        pIconCell->SetAttribute(DUI_T("width"), DUI_T("32"));
        pIconCell->SetAttribute(DUI_T("height"), DUI_T("stretch"));
        pIconCell->SetAttribute(DUI_T("child_align"), DUI_T("hcenter,vcenter"));
        pIconCell->SetMouseEnabled(false);
        pIconCell->AddItem(entry.isDir ? MakeFolderIcon(this, 20)
                                       : MakeFileIcon(this, 20));
        pRow->AddItem(pIconCell);

        ui::Label* pName = new ui::Label(this);
        pName->SetText(entry.name);
        pName->SetAttribute(DUI_T("font"), DUI_T("system_14"));
        pName->SetStateTextColor(ui::kControlStateNormal,
                                 entry.isDir ? kAccent : kTextDark);
        pName->SetAttribute(DUI_T("text_align"), DUI_T("left,vcenter"));
        pName->SetAttribute(DUI_T("width"), DUI_T("stretch"));
        pName->SetAttribute(DUI_T("height"), DUI_T("stretch"));
        pName->SetMouseEnabled(false);
        pRow->AddItem(pName);

        ui::Label* pSize = new ui::Label(this);
        pSize->SetText(entry.isDir ? DUI_T("—") : FormatSize(entry.size));
        pSize->SetAttribute(DUI_T("font"), DUI_T("system_13"));
        pSize->SetStateTextColor(ui::kControlStateNormal, kTextHint);
        pSize->SetAttribute(DUI_T("text_align"), DUI_T("right,vcenter"));
        pSize->SetAttribute(DUI_T("width"), DUI_T("110"));
        pSize->SetAttribute(DUI_T("height"), DUI_T("stretch"));
        pSize->SetMouseEnabled(false);
        pRow->AddItem(pSize);

        ui::Label* pTime = new ui::Label(this);
        pTime->SetText(entry.mtime);
        pTime->SetAttribute(DUI_T("font"), DUI_T("system_13"));
        pTime->SetStateTextColor(ui::kControlStateNormal, kTextHint);
        pTime->SetAttribute(DUI_T("text_align"), DUI_T("right,vcenter"));
        pTime->SetAttribute(DUI_T("width"), DUI_T("160"));
        pTime->SetAttribute(DUI_T("height"), DUI_T("stretch"));
        pTime->SetMouseEnabled(false);
        pRow->AddItem(pTime);

        pRow->AttachClick([this, i](const ui::EventArgs& args) {
            SelectRow(i, args.modifierKey);
            return true;
        });
        pRow->AttachDoubleClick([this, i](const ui::EventArgs&) {
            OpenRow(i);
            return true;
        });
        pRow->AttachRClick([this, i](const ui::EventArgs& args) {
            ShowEntryMenu(i, args.ptMouse);
            return true;
        });

        m_pFileList->AddItem(pRow);
    }

    UpdateStatus();
}

void FilesForm::SelectRow(size_t index, unsigned int modifiers)
{
    if (index >= m_entries.size()) {
        return;
    }
    HideContextMenu();

    if ((modifiers & ui::kControl) != 0 || (modifiers & ui::kShift) != 0) {
        if ((modifiers & ui::kShift) != 0) {
            // A range from the anchor, the way both Explorer and Files do it.
            const size_t from = std::min(m_anchor, index);
            const size_t to = std::max(m_anchor, index);
            m_selected.clear();
            for (size_t i = from; i <= to; ++i) {
                m_selected.push_back(i);
            }
        } else if (IsSelected(index)) {
            m_selected.erase(std::find(m_selected.begin(), m_selected.end(), index));
            m_anchor = index;
        } else {
            m_selected.push_back(index);
            m_anchor = index;
        }
    } else {
        m_selected.assign(1, index);
        m_anchor = index;
    }

    ReloadList();
}

void FilesForm::OpenRow(size_t index)
{
    if (index < m_entries.size()) {
        HideContextMenu();
        OpenEntry(m_entries[index]);
    }
}

void FilesForm::ShowEntryMenu(size_t index, const ui::UiPoint& pt)
{
    if (m_pContextMenu == nullptr || index >= m_entries.size()) {
        return;
    }
    m_contextIndex = index;
    m_selected.assign(1, index);
    m_anchor = index;

    m_pContextMenu->RemoveAllItems();
    const Entry& entry = m_entries[index];

    struct Action { const char* label; int id; };
    const Action actions[] = {
        { "打开", 0 },
        { "在终端中打开", 1 },
        { "移到废纸篓", 2 },
    };
    for (const Action& action : actions) {
        ui::Button* pItem = new ui::Button(this);
        pItem->SetText(DString(action.label));
        pItem->SetAttribute(DUI_T("font"), DUI_T("system_14"));
        pItem->SetStateTextColor(ui::kControlStateNormal,
                                 action.id == 2 ? DUI_T("#FFFF453A") : kTextBody);
        pItem->SetAttribute(DUI_T("text_align"), DUI_T("left,vcenter"));
        pItem->SetAttribute(DUI_T("text_padding"), DUI_T("10,0,10,0"));
        pItem->SetAttribute(DUI_T("width"), DUI_T("stretch"));
        pItem->SetAttribute(DUI_T("height"), DUI_T("30"));
        pItem->SetAttribute(DUI_T("cursor_type"), DUI_T("hand"));
        pItem->SetStateColor(ui::kControlStateNormal, kTransparent);
        pItem->SetStateColor(ui::kControlStateHot, kRowHot);
        SetRadius(pItem, 6, true);
        const size_t row = index;
        const int id = action.id;
        pItem->AttachClick([this, row, id](const ui::EventArgs&) {
            HideContextMenu();
            if (row >= m_entries.size()) {
                return true;
            }
            if (id == 0) {
                OpenEntry(m_entries[row]);
            } else if (id == 1) {
                DString cmd = m_curDir;
                ShellQuote(cmd);
                LaunchCommand(DString(DUI_T("wayst -e sh -c 'cd ")) + cmd +
                              DUI_T(" && exec bash'"));
            } else {
                TrashEntry(row);
            }
            return true;
        });
        m_pContextMenu->AddItem(pItem);
    }

    ui::UiRect client;
    GetClientRect(client);
    const int width = 180;
    const int height = 6 * 2 + 30 * 3;
    int px = pt.x;
    int py = pt.y;
    if (px + width > client.right) px = client.right - width - 4;
    if (py + height > client.bottom) py = client.bottom - height - 4;
    if (px < 4) px = 4;
    if (py < 4) py = 4;
    m_pContextMenu->SetAttribute(DUI_T("height"), Num(height));
    m_pContextMenu->SetPos(ui::UiRect(px, py, px + width, py + height));
    m_pContextMenu->SetVisible(true);
    m_pContextMenu->Invalidate();
}

void FilesForm::HideContextMenu()
{
    if (m_pContextMenu != nullptr && m_pContextMenu->IsVisible()) {
        m_pContextMenu->SetVisible(false);
        InvalidateAll();
    }
}

void FilesForm::TrashEntry(size_t index)
{
    if (index >= m_entries.size()) {
        return;
    }
    const char* home = std::getenv("HOME");
    if (home == nullptr) {
        return;
    }
    DString source = m_curDir;
    if (!source.empty() && source.back() != DUI_T('/')) {
        source += DUI_T('/');
    }
    source += m_entries[index].name;
    DString trashDir(home);
    trashDir += DUI_T("/.Trash");
    DString target = trashDir + DUI_T("/") + m_entries[index].name;

    DString quotedSource = source;
    DString quotedTarget = target;
    DString quotedTrashDir = trashDir;
    ShellQuote(quotedSource);
    ShellQuote(quotedTarget);
    ShellQuote(quotedTrashDir);

    // Never clobber something already in the trash: a plain mv would.
    LaunchCommand(DString(DUI_T("mkdir -p ")) + quotedTrashDir + DUI_T(" && mv -n ") +
                  quotedSource + DUI_T(" ") + quotedTarget);
    Refresh();
}

void FilesForm::NewFolder()
{
    std::vector<Entry> existing;
    ListDirectory(m_curDir, existing);
    DString name = DUI_T("新建文件夹");
    bool taken = true;
    int suffix = 1;
    while (taken) {
        taken = false;
        for (const Entry& entry : existing) {
            if (entry.name == name) {
                taken = true;
                break;
            }
        }
        if (taken) {
            DString candidate = DUI_T("新建文件夹 ");
            candidate += std::to_string(++suffix).c_str();
            name = candidate;
        }
    }

    DString path = m_curDir;
    if (!path.empty() && path.back() != DUI_T('/')) {
        path += DUI_T('/');
    }
    path += name;
    DString quoted = path;
    ShellQuote(quoted);
    LaunchCommand(DString(DUI_T("mkdir -p ")) + quoted);
    Refresh();
}

void FilesForm::UpdateBreadcrumb()
{
    if (m_pBreadcrumb == nullptr) {
        return;
    }

    // Split the path into segments, each of which navigates to its prefix.
    std::vector<std::pair<DString, DString>> crumbs;   // label, full path
    crumbs.push_back({ DUI_T("文件系统"), DString(DUI_T("/")) });
    DString accumulated(DUI_T("/"));
    DString rest = m_curDir;
    if (!rest.empty() && rest[0] == DUI_T('/')) {
        rest = rest.substr(1);
    }
    while (!rest.empty()) {
        const size_t slash = rest.find(DUI_T('/'));
        const DString segment = (slash == DString::npos) ? rest : rest.substr(0, slash);
        if (!segment.empty()) {
            if (accumulated.size() > 1) {
                accumulated += DUI_T('/');
            }
            accumulated += segment;
            crumbs.push_back({ segment, accumulated });
        }
        if (slash == DString::npos) {
            break;
        }
        rest = rest.substr(slash + 1);
    }

    // Keep the tail when the path is deeper than the row has slots for.
    size_t first = 0;
    if (crumbs.size() > static_cast<size_t>(kCrumbSlots)) {
        first = crumbs.size() - kCrumbSlots;
    }
    const size_t shown = crumbs.size() - first;

    for (int i = 0; i < kCrumbSlots; ++i) {
        ui::Button* pCrumb = m_pCrumbButtons[i];
        if (pCrumb == nullptr) {
            continue;
        }
        if (static_cast<size_t>(i) < shown) {
            const std::pair<DString, DString>& crumb = crumbs[first + i];
            DString label = crumb.first;
            if (i == 0 && first > 0) {
                label = DString(DUI_T("…/")) + label;
            }
            pCrumb->SetText(label);
            m_crumbTargets[i] = crumb.second;
            const bool last = (static_cast<size_t>(i) + 1 == shown);
            pCrumb->SetStateTextColor(ui::kControlStateNormal,
                                      last ? kTextDark : kTextHint);
        } else {
            pCrumb->SetText(DUI_T(""));
            m_crumbTargets[i] = "";
        }
    }

    // Highlight the deepest place the current directory sits under. Only one
    // of them: "/" is a prefix of everything, and /home/shxu is a prefix of
    // the Desktop, so a plain prefix test lights half the sidebar.
    size_t bestLength = 0;
    if (!m_curDir.empty()) {
        for (ui::Button* pPlace : m_pPlaceButtons) {
            const DString target = pPlace->GetToolTipText();
            if (target.empty() || target.size() > m_curDir.size()) {
                continue;
            }
            if (m_curDir.compare(0, target.size(), target) != 0) {
                continue;
            }
            const bool atBoundary = target.size() == m_curDir.size() ||
                (target != DUI_T("/") && m_curDir[target.size()] == DUI_T('/'));
            if (atBoundary && target.size() > bestLength) {
                bestLength = target.size();
            }
        }
    }
    for (ui::Button* pPlace : m_pPlaceButtons) {
        const bool here = bestLength > 0 &&
            pPlace->GetToolTipText().size() == bestLength;
        pPlace->SetStateColor(ui::kControlStateNormal,
                              here ? kRowSelected : kTransparent);
    }

    if (m_pBackButton != nullptr) {
        m_pBackButton->SetEnabled(!m_history.empty());
    }
    if (m_pForwardButton != nullptr) {
        m_pForwardButton->SetEnabled(!m_forward.empty());
    }
}

void FilesForm::UpdateStatus()
{
    if (m_pStatusLabel != nullptr) {
        size_t dirs = 0;
        for (const Entry& entry : m_entries) {
            if (entry.isDir) {
                ++dirs;
            }
        }
        DString text = Num(static_cast<int>(m_entries.size())) + DUI_T(" 个项目");
        if (dirs > 0) {
            text += DUI_T("（") + Num(static_cast<int>(dirs)) + DUI_T(" 个文件夹）");
        }
        m_pStatusLabel->SetText(text);
    }
    if (m_pSelectionLabel != nullptr) {
        m_pSelectionLabel->SetText(m_selected.empty()
            ? DString(DUI_T(""))
            : DString(DUI_T("已选 ")) + Num(static_cast<int>(m_selected.size())) + DUI_T(" 项"));
    }
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
