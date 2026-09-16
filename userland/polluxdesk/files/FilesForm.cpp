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
const U8String kBarBg        = "#F2FFFFFF";
const U8String kBarBorder    = "#33000000";
const U8String kTransparent  = "#00000000";
const U8String kSidebarBg    = "#E8F2F3F7";
const U8String kListBg       = "#F9FFFFFF";
const U8String kRowSelected  = "#330A84FF";
const U8String kTextDark     = "#FF1D1D1F";
const U8String kTextBody     = "#FF3A3A3C";
const U8String kTextHint     = "#FF8E8E93";
const U8String kTextOnAccent = "#FFFFFFFF";
const U8String kAccent       = "#FF0A84FF";
const U8String kHairline     = "#22000000";
const U8String kFolderColor   = "#FF4C9FDB";

// Hover, deliberately unused.
//
// A pointer move marks the control it passed over as hot but repaints nothing,
// nothing ever clears that mark -- dui's window-level "the pointer left" hides
// the tooltip and stops there, it does not reset the control -- and so the
// highlight of everything the pointer has ever crossed appears at the next
// repaint and then stays.  The result is a trail of blue boxes behind the
// mouse that no amount of moving away removes.
//
// So no control here sets a hot colour: what the pointer does is not shown,
// and clicks are answered by the pressed state and by the selection, which the
// app sets itself and can therefore also clear.  Where a control wants a
// selected look -- the sidebar places, the file rows -- every state is set to
// that look at once (see SetAllStateColors below), so a control left in any of
// them still shows what it means and nothing else can repaint it.

const int kRowHeight  = 30;
const int kCrumbW     = 72;   // one path segment in the breadcrumb
const int kIconTileW  = 100;
const int kIconTileH  = 96;
const int kToolH      = 34;   // toolbar button height
const int kArrowW     = 46;   // back / forward / up: a wider target than a word

// One colour for every state of a control.
//
// Which state a control is left in cannot be relied on here (see the note on
// hover above), and dui's own fallback chain -- pushed falls back to hot, hot
// to normal -- means a state that is left set can outlive the reason it was
// set. Painting all four the same makes the control look the same however it
// is left: what it should look like is then the app's decision, taken when the
// selection changes, and nothing else can repaint it a different colour.
void SetAllStateColors(ui::Control* pControl, const U8String& colour)
{
    pControl->SetStateColor(ui::kControlStateNormal, colour);
    pControl->SetStateColor(ui::kControlStateHot, colour);
    pControl->SetStateColor(ui::kControlStatePushed, colour);
    pControl->SetStateColor(ui::kControlStateDisabled, colour);
}

U8String FormatSize(long long bytes)
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
    return U8String(buf);
}

// Extensions opened in a terminal text editor; everything else also falls
// back to the editor (there is no MIME handler database on this minimal
// system yet), except executables which are run directly.
bool IsTextExtension(const U8String& name)
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
    pControl->SetAttribute("border_round",
        ui::StringUtil::Printf("%d,%d", radius, radius));
}

U8String Num(int value)
{
    return ui::StringUtil::Printf("%d", value);
}

// Make a scroll pane re-measure itself after its contents have changed.
//
// dui works out a scroll box's range while it arranges the box's children, and
// adding children does not arrange anything. So a box filled after the window
// was laid out never learns it holds more than fits: its bar is never given a
// range, and ScrollBar::SetScrollRange's "range is still zero" branch leaves
// the bar hidden with an empty rect for good -- a blank strip where the bar
// should be, that no click can ever reach. Re-setting the box's own rectangle
// re-runs the arrange, and with it the range.
void SyncScrollRange(ui::VScrollBox* pBox)
{
    if (pBox == nullptr) {
        return;
    }
    pBox->SetPos(pBox->GetPos());

    // And make it opaque. The theme's class carries fade_alpha, whose
    // animation dui only *plays* when the visibility actually changes -- here
    // it does not, so the animation is stopped instead and the bar is left at
    // the alpha its fade would have started from, which is nothing. Visible,
    // sized, and invisible: that is the blank strip the pane showed.
    ui::ScrollBar* pBar = pBox->GetVScrollBar();
    if (pBar != nullptr) {
        pBar->SetAlpha(255);
    }
}

// A hairline used between the panes.
ui::Control* MakeHairline(ui::Window* pWindow, bool vertical)
{
    ui::Control* pLine = new ui::Control(pWindow);
    pLine->SetAttribute("width", vertical ? "1" : "stretch");
    pLine->SetAttribute("height", vertical ? "stretch" : "1");
    pLine->SetBkColor(kHairline);
    pLine->SetMouseEnabled(false);
    return pLine;
}

// A small round-cornered glyph box: the folder tab and the document fold are
// the only detail that survives at this size.
ui::VBox* MakeFolderIcon(ui::Window* pWindow, int size)
{
    ui::VBox* pIcon = new ui::VBox(pWindow);
    pIcon->SetAttribute("width", Num(size * 13 / 10));
    pIcon->SetAttribute("height", Num(size));
    // An HBox aligns a child vertically by the child's own valign -- the bar's
    // child_align only reaches the horizontal axis -- so the glyph has to ask
    // for its own centring, or it rides at the top of the cell.
    pIcon->SetAttribute("valign", "center");
    pIcon->SetBkColor(kFolderColor);
    pIcon->SetBkColor2("#FF3E8ECA");
    pIcon->SetBkColor2Direction("1");
    SetRadius(pIcon, std::max(3, size / 5), false);
    pIcon->SetMouseEnabled(false);

    ui::Control* pTab = new ui::Control(pWindow);
    pTab->SetAttribute("width", Num(size * 3 / 5));
    pTab->SetAttribute("height", Num(size / 5));
    pTab->SetAttribute("margin", "-1,-2,0,0");
    pTab->SetAttribute("halign", "left");
    pTab->SetBkColor("#FF2F7AB8");
    pTab->SetMouseEnabled(false);
    pIcon->AddItem(pTab);
    return pIcon;
}

ui::VBox* MakeFileIcon(ui::Window* pWindow, int size)
{
    ui::VBox* pIcon = new ui::VBox(pWindow);
    pIcon->SetAttribute("width", Num(size * 9 / 10));
    pIcon->SetAttribute("height", Num(size));
    pIcon->SetAttribute("valign", "center");   // see MakeFolderIcon
    pIcon->SetBkColor("#FFFFFFFF");
    pIcon->SetBorderColor("#FFC9C9CE");
    pIcon->SetAttribute("border_size", "1");
    SetRadius(pIcon, std::max(2, size / 7), false);
    pIcon->SetAttribute("child_align", "vcenter");
    pIcon->SetMouseEnabled(false);
    for (int i = 0; i < 3; ++i) {
        if (i > 0) {
            ui::Control* pGap = new ui::Control(pWindow);
            pGap->SetAttribute("height", "2");
            pGap->SetMouseEnabled(false);
            pIcon->AddItem(pGap);
        }
        ui::Control* pLine = new ui::Control(pWindow);
        pLine->SetAttribute("height", "1");
        pLine->SetAttribute("width", "stretch");
        pLine->SetAttribute("margin", "2,0,2,0");
        pLine->SetAttribute("halign", "center");
        pLine->SetBkColor("#FFC7C7CC");
        pLine->SetMouseEnabled(false);
        pIcon->AddItem(pLine);
    }
    return pIcon;
}

// Sidebar glyphs, drawn from boxes the way the row icons are: the embedded
// font has no symbol set worth relying on, and a folder, a house, a can and a
// disk are all recognisable at this size from two or three rectangles.
ui::VBox* MakePlaceIcon(ui::Window* pWindow, const U8String& kind)
{
    ui::VBox* pIcon = new ui::VBox(pWindow);
    pIcon->SetAttribute("width", "18");
    pIcon->SetAttribute("height", "16");
    pIcon->SetAttribute("valign", "center");
    pIcon->SetMouseEnabled(false);

    if (kind == "folder") {
        pIcon->SetBkColor(kFolderColor);
        pIcon->SetBkColor2("#FF3E8ECA");
        pIcon->SetBkColor2Direction("1");
        pIcon->SetAttribute("height", "13");
        SetRadius(pIcon, 3, false);
        ui::Control* pTab = new ui::Control(pWindow);
        pTab->SetAttribute("width", "8");
        pTab->SetAttribute("height", "3");
        pTab->SetAttribute("margin", "-1,-2,0,0");
        pTab->SetAttribute("halign", "left");
        pTab->SetBkColor("#FF2F7AB8");
        pTab->SetMouseEnabled(false);
        pIcon->AddItem(pTab);
        return pIcon;
    }

    if (kind == "house") {
        ui::Control* pRoof = new ui::Control(pWindow);
        pRoof->SetAttribute("width", "18");
        pRoof->SetAttribute("height", "5");
        pRoof->SetBkColor(kFolderColor);
        pRoof->SetMouseEnabled(false);
        SetRadius(pRoof, 2, false);
        pIcon->AddItem(pRoof);

        ui::Control* pBody = new ui::Control(pWindow);
        pBody->SetAttribute("width", "14");
        pBody->SetAttribute("height", "9");
        pBody->SetAttribute("margin", "0,1,0,0");
        pBody->SetAttribute("halign", "center");
        pBody->SetBkColor("#FF3E8ECA");
        pBody->SetMouseEnabled(false);
        SetRadius(pBody, 2, false);
        pIcon->AddItem(pBody);
        return pIcon;
    }

    if (kind == "trash") {
        ui::Control* pLid = new ui::Control(pWindow);
        pLid->SetAttribute("width", "14");
        pLid->SetAttribute("height", "3");
        pLid->SetAttribute("halign", "center");
        pLid->SetBkColor(kTextHint);
        pLid->SetMouseEnabled(false);
        SetRadius(pLid, 1, false);
        pIcon->AddItem(pLid);

        ui::Control* pBody = new ui::Control(pWindow);
        pBody->SetAttribute("width", "11");
        pBody->SetAttribute("height", "11");
        pBody->SetAttribute("margin", "0,1,0,0");
        pBody->SetAttribute("halign", "center");
        pBody->SetBkColor(kTextHint);
        pBody->SetMouseEnabled(false);
        SetRadius(pBody, 2, false);
        pIcon->AddItem(pBody);
        return pIcon;
    }

    // filesystem: a drive
    pIcon->SetBkColor(kTextHint);
    pIcon->SetAttribute("height", "12");
    SetRadius(pIcon, 2, false);
    ui::Control* pSlot = new ui::Control(pWindow);
    pSlot->SetAttribute("width", "14");
    pSlot->SetAttribute("height", "3");
    pSlot->SetAttribute("margin", "0,2,0,0");
    pSlot->SetAttribute("halign", "center");
    pSlot->SetBkColor(kListBg);
    pSlot->SetMouseEnabled(false);
    pIcon->AddItem(pSlot);
    return pIcon;
}

// A flat toolbar button with a glyph, as the browser's own toolbar uses.
ui::Button* MakeToolButton(ui::Window* pWindow, const U8String& glyph,
                           const U8String& tip, int width,
                           const U8String& font = "system_18")
{
    ui::Button* pButton = new ui::Button(pWindow);
    pButton->SetText(glyph);
    pButton->SetAttribute("font", font);
    pButton->SetAttribute("width", Num(width));
    pButton->SetAttribute("height", Num(kToolH));
    pButton->SetAttribute("margin", "0,0,4,0");
    pButton->SetAttribute("text_align", "hcenter,vcenter");
    pButton->SetAttribute("cursor_type", "hand");
    // The pressed flash is the only state that shows, which is why it is the
    // one that keeps a colour.
    SetAllStateColors(pButton, kTransparent);
    pButton->SetStateColor(ui::kControlStatePushed, kRowSelected);
    pButton->SetStateTextColor(ui::kControlStateNormal, kTextBody);
    pButton->SetStateTextColor(ui::kControlStateDisabled, kTextHint);
    SetRadius(pButton, 6, true);
    pButton->SetToolTipText(tip);
    return pButton;
}

// One line of text in the status bar.
ui::Label* MakeStatusText(ui::Window* pWindow, const U8String& text,
                          const U8String& colour, const U8String& align)
{
    ui::Label* pLabel = new ui::Label(pWindow);
    pLabel->SetText(text);
    pLabel->SetAttribute("font", "system_12");
    pLabel->SetStateTextColor(ui::kControlStateNormal, colour);
    pLabel->SetAttribute("text_align", align);
    pLabel->SetAttribute("height", "stretch");
    pLabel->SetAttribute("width", "stretch");
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

U8String FilesForm::GetSkinFolder()
{
    return "polluxdesk";
}

U8String FilesForm::GetSkinFile()
{
    return "files.xml";
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

    // The pane scrolls on the wheel out of the box; the bar is shown outright
    // rather than left to the theme's fade.
    //
    // The theme's vscrollbar class sets fade_alpha and the bar starts
    // *hidden*: ScrollBar::SetScrollRange hides it while the range is still
    // zero, which it is at this point because the directory has not been read
    // yet. Nothing brings it back -- the only re-show in dui is gated on
    // "not auto-hide" -- and the box goes on reserving its twelve pixels, so
    // the window shows a blank strip with no bar to grab. Auto-hide off is
    // what makes that branch reachable, and then SetScrollRange shows it as
    // soon as the first listing gives it a range.
    if (m_pFileList != nullptr) {
        ui::ScrollBar* pBar = m_pFileList->GetVScrollBar();
        if (pBar != nullptr) {
            pBar->SetClass("vscrollbar");
            pBar->SetAutoHideScroll(false);
        }
    }

    // Open where we were asked to, falling back rather than leaving the
    // window empty: the dock asks for the trash, which does not exist until
    // something has been thrown away.
    if (m_startDir.empty() || !Navigate(m_startDir)) {
        const char* home = std::getenv("HOME");
        if (home == nullptr || !Navigate(U8String(home))) {
            Navigate("/");
        }
    }

    // That listing was built while the window was still unlaid-out: the pane
    // has no rectangle yet, so syncing its range there does nothing. One
    // deferred pass, once the layout has given the pane its size, is what
    // shows the bar for the opening directory. Listings made later are synced
    // where they are built, in ReloadList.
    ui::GlobalManager::Instance().Timer().AddTimer(GetWeakFlag(), [this]() {
        SyncScrollRange(m_pFileList);
    }, 200, 1);
}

void FilesForm::BuildUi()
{
    ui::VBox* pRoot = new ui::VBox(this);
    pRoot->SetBkColor(kListBg);
    pRoot->SetBorderColor(kTransparent);
    pRoot->SetAttribute("border_size", "0");
    pRoot->SetAttribute("padding", "0,0,0,0");

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
    pToolbar->SetAttribute("height", "auto");
    pToolbar->SetAttribute("width", "stretch");
    pToolbar->SetAttribute("padding", "10,8,10,8");
    pToolbar->SetBkColor(kBarBg);

    // Row one is the buttons. They get a row to themselves rather than
    // sharing with the path: a row that holds both a stretch item and several
    // fixed ones lays out unpredictably, and whatever lost the width was
    // simply not drawn.
    ui::HBox* pRow = new ui::HBox(this);
    pRow->SetAttribute("width", "stretch");
    pRow->SetAttribute("height", Num(kToolH));

    // The three arrows get more room than the word buttons next to them: a
    // single glyph in a 34px square was a small target for the two controls
    // used most.
    m_pBackButton = MakeToolButton(this, "←", "后退", kArrowW,
                                  "system_24");
    m_pBackButton->AttachClick([this](const ui::EventArgs&) {
        NavigateBack();
        return true;
    });
    pRow->AddItem(m_pBackButton);

    m_pForwardButton = MakeToolButton(this, "→", "前进", kArrowW,
                                     "system_24");
    m_pForwardButton->AttachClick([this](const ui::EventArgs&) {
        NavigateForward();
        return true;
    });
    pRow->AddItem(m_pForwardButton);

    ui::Button* pUp = MakeToolButton(this, "↑", "上一级", kArrowW,
                                      "system_24");
    pUp->AttachClick([this](const ui::EventArgs&) {
        NavigateUp();
        return true;
    });
    pRow->AddItem(pUp);

    ui::Control* pSpacer = new ui::Control(this);
    pSpacer->SetAttribute("width", "stretch");
    pSpacer->SetMouseEnabled(false);
    pRow->AddItem(pSpacer);

    // Text rather than symbols: the embedded font has the arrows but not most
    // of the geometric shapes, which come out as empty boxes.
    ui::Button* pNewFolder = MakeToolButton(this, "新建", "新建文件夹", 52);
    pNewFolder->AttachClick([this](const ui::EventArgs&) {
        NewFolder();
        return true;
    });
    pRow->AddItem(pNewFolder);

    ui::Button* pView = MakeToolButton(this, "视图", "切换列表 / 图标", 52);
    pView->AttachClick([this](const ui::EventArgs&) {
        SetIconView(!m_iconView);
        return true;
    });
    pRow->AddItem(pView);

    ui::Button* pRefresh = MakeToolButton(this, "刷新", "刷新", 52);
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
    m_pBreadcrumb->SetAttribute("width", "stretch");
    m_pBreadcrumb->SetAttribute("height", "28");
    m_pBreadcrumb->SetAttribute("margin", "0,6,0,0");
    for (int i = 0; i < kCrumbSlots; ++i) {
        if (i > 0) {
            ui::Label* pSep = new ui::Label(this);
            pSep->SetText("›");
            pSep->SetAttribute("font", "system_13");
            pSep->SetStateTextColor(ui::kControlStateNormal, kTextHint);
            pSep->SetAttribute("width", "14");
            pSep->SetAttribute("height", "stretch");
            pSep->SetAttribute("text_align", "hcenter,vcenter");
            pSep->SetMouseEnabled(false);
            m_pBreadcrumb->AddItem(pSep);
            // Kept so a shorter path can take the separator with it; a trail
            // of "›" pointing at nothing is what a path that ends early looks
            // like otherwise.
            m_pCrumbSeps.push_back(pSep);
        }
        ui::Button* pCrumb = new ui::Button(this);
        pCrumb->SetAttribute("font", "system_13");
        pCrumb->SetAttribute("text_align", "hcenter,vcenter");
        pCrumb->SetAttribute("text_padding", "4,0,4,0");
        pCrumb->SetAttribute("height", "stretch");
        pCrumb->SetAttribute("width", Num(kCrumbW));
        pCrumb->SetAttribute("cursor_type", "hand");
        // The crumb is a word in a path, not a button: no hover box, and
        // nothing left behind by a click either.
        SetAllStateColors(pCrumb, kTransparent);
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
    pBody->SetAttribute("width", "stretch");
    pBody->SetAttribute("height", "stretch");
    BuildSidebar(pBody);
    pBody->AddItem(MakeHairline(this, true));
    BuildContent(pBody);
    pRoot->AddItem(pBody);
}

void FilesForm::BuildSidebar(ui::HBox* pParent)
{
    m_pSidebar = new ui::VBox(this);
    m_pSidebar->SetAttribute("width", "180");
    m_pSidebar->SetAttribute("height", "stretch");
    m_pSidebar->SetAttribute("padding", "8,10,8,10");
    m_pSidebar->SetBkColor(kSidebarBg);

    ui::Label* pTitle = new ui::Label(this);
    pTitle->SetText("位置");
    pTitle->SetAttribute("font", "system_12");
    pTitle->SetStateTextColor(ui::kControlStateNormal, kTextHint);
    pTitle->SetAttribute("text_align", "left,vcenter");
    pTitle->SetAttribute("height", "24");
    pTitle->SetMouseEnabled(false);
    m_pSidebar->AddItem(pTitle);

    const char* home = std::getenv("HOME");
    const U8String homeDir(home != nullptr ? home : "/");

    std::vector<Place> places;
    places.push_back({ "主目录", homeDir, "house" });
    struct Shortcut { const char* label; const char* sub; };
    const Shortcut shortcuts[] = {
        { "桌面", "Desktop" }, { "文档", "Documents" }, { "下载", "Downloads" },
        { "音乐", "Music" }, { "图片", "Pictures" }, { "视频", "Videos" },
    };
    for (const Shortcut& shortcut : shortcuts) {
        const U8String path = homeDir + "/" + U8String(shortcut.sub);
        struct stat st;
        if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            places.push_back({ shortcut.label, path, "folder" });
        }
    }
    places.push_back({ "废纸篓", homeDir + "/.Trash", "trash" });
    places.push_back({ "文件系统", "/", "disk" });

    for (const Place& place : places) {
        // An icon and a label rather than the button's own text, so each place
        // gets the glyph it deserves.
        ui::ButtonHBox* pItem = new ui::ButtonHBox(this);
        pItem->SetAttribute("width", "stretch");
        pItem->SetAttribute("height", "30");
        pItem->SetAttribute("margin", "0,1,0,1");
        pItem->SetAttribute("cursor_type", "hand");
        // No hover, and no press flash either: the sidebar's one highlight is
        // the place the window is at, which UpdateBreadcrumb sets and clears.
        SetAllStateColors(pItem, kTransparent);
        SetRadius(pItem, 6, true);

        ui::HBox* pIconCell = new ui::HBox(this);
        pIconCell->SetAttribute("width", "30");
        pIconCell->SetAttribute("height", "stretch");
        pIconCell->SetAttribute("child_align", "hcenter,vcenter");
        pIconCell->SetMouseEnabled(false);
        pIconCell->AddItem(MakePlaceIcon(this, U8String(place.icon)));
        pItem->AddItem(pIconCell);

        ui::Label* pText = new ui::Label(this);
        pText->SetText(U8String(place.label));
        pText->SetAttribute("font", "system_14");
        pText->SetAttribute("text_align", "left,vcenter");
        pText->SetStateTextColor(ui::kControlStateNormal, kTextBody);
        pText->SetAttribute("width", "stretch");
        pText->SetAttribute("height", "stretch");
        pText->SetMouseEnabled(false);
        pItem->AddItem(pText);
        const U8String path = place.path;
        pItem->AttachClick([this, path](const ui::EventArgs&) {
            Navigate(path);
            return true;
        });
        m_pSidebar->AddItem(pItem);
        m_pPlaceButtons.push_back(pItem);
        m_placePaths.push_back(place.path);
        if (place.path == m_curDir) {
            SetAllStateColors(pItem, kRowSelected);
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
    pContent->SetAttribute("width", "stretch");
    pContent->SetAttribute("height", "stretch");
    // Inset on the right so the list's scrollbar does not sit hard against the
    // window edge: the compositor treats the outermost pixels of a window as a
    // resize hotspot, and a bar drawn there is under the pointer's own edge
    // rather than under the app. The headings move with the list, so the
    // columns stay lined up.
    pContent->SetAttribute("padding", "0,0,8,0");

    // Column headings. Clicking one sorts by it and clicks again reverse it,
    // which is the one thing every file browser's list view shares.
    m_pHeaderRow = new ui::HBox(this);
    m_pHeaderRow->SetAttribute("width", "stretch");
    m_pHeaderRow->SetAttribute("height", "26");
    m_pHeaderRow->SetAttribute("padding", "8,0,8,0");
    m_pHeaderRow->SetBkColor(kBarBg);

    const char* kColumnNames[] = { "名称", "大小", "修改日期" };
    const int kColumnWidths[] = { 0, 110, 160 };   // 0 = stretch
    for (int i = 0; i < 3; ++i) {
        ui::Button* pColumn = new ui::Button(this);
        pColumn->SetAttribute("font", "system_12");
        pColumn->SetStateTextColor(ui::kControlStateNormal, kTextHint);
        pColumn->SetAttribute("text_align", "left,vcenter");
        pColumn->SetAttribute("text_padding", "6,0,6,0");
        pColumn->SetAttribute("height", "stretch");
        pColumn->SetAttribute("width",
            kColumnWidths[i] == 0 ? "stretch" : Num(kColumnWidths[i]));
        pColumn->SetAttribute("cursor_type", "hand");
        SetAllStateColors(pColumn, kTransparent);
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
        pColumn->SetText(U8String(kColumnNames[i]));
        m_pHeaderRow->AddItem(pColumn);
        m_pColumnButtons[i] = pColumn;
    }
    pContent->AddItem(m_pHeaderRow);

    m_pFileList = new ui::VScrollBox(this);
    m_pFileList->SetAttribute("width", "stretch");
    m_pFileList->SetAttribute("height", "stretch");
    m_pFileList->SetBkColor(kListBg);
    m_pFileList->SetBorderColor(kTransparent);
    // Without this the pane only moves on a drag: dui builds a scroll box's
    // bar on request rather than by default.
    m_pFileList->EnableScrollBar(true, false);
    m_pFileList->SetVerScrollUnitPixels(48, true);

    pContent->AddItem(m_pFileList);

    pParent->AddItem(pContent);
}

void FilesForm::BuildStatusBar(ui::VBox* pRoot)
{
    ui::HBox* pStatus = new ui::HBox(this);
    pStatus->SetAttribute("height", "30");
    pStatus->SetAttribute("width", "stretch");
    pStatus->SetAttribute("padding", "12,0,12,0");
    pStatus->SetBkColor(kBarBg);
    pStatus->AddItem(MakeHairline(this, false));

    ui::HBox* pRow = new ui::HBox(this);
    pRow->SetAttribute("width", "stretch");
    pRow->SetAttribute("height", "stretch");
    pRow->AddItem(nullptr);
    pRow->RemoveAllItems();

    // The path takes whatever room is left; the counts get a fixed column so
    // neither squeezes the other off the end.
    m_pStatusLabel = MakeStatusText(this, "", kTextBody, "left,vcenter");
    m_pStatusLabel->SetAttribute("width", "stretch");
    pRow->AddItem(m_pStatusLabel);
    m_pSelectionLabel = MakeStatusText(this, "", kTextHint, "right,vcenter");
    m_pSelectionLabel->SetAttribute("width", "200");
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
    m_pContextMenu->SetAttribute("border_size", "1");
    m_pContextMenu->SetAttribute("width", "180");
    m_pContextMenu->SetAttribute("padding", "6,6,6,6");
    SetRadius(m_pContextMenu, 9, false);
    m_pContextMenu->SetPaintOrder(200);
    m_pContextMenu->SetVisible(false);
}

bool FilesForm::Navigate(const U8String& path)
{
    std::vector<Entry> entries;
    if (!ListDirectory(path, entries)) {
        if (m_pStatusLabel != nullptr) {
            m_pStatusLabel->SetText("无法打开目录：" + path);
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
    const U8String previous = m_history.back();
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
    const U8String next = m_forward.back();
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
    if (m_curDir.empty() || m_curDir == "/") {
        return;
    }
    U8String parent = m_curDir;
    while (parent.size() > 1 && parent.back() == '/') {
        parent.pop_back();
    }
    const size_t slash = parent.find_last_of('/');
    parent = (slash == U8String::npos || slash == 0) ? U8String("/")
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
    if (index >= m_entries.size()) {
        return false;
    }
    const U8String& name = m_entries[index].name;
    return std::find(m_selected.begin(), m_selected.end(), name) != m_selected.end();
}

bool FilesForm::ListDirectory(const U8String& dir, std::vector<Entry>& out)
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

        U8String full = dir;
        if (!full.empty() && full.back() != '/') {
            full += '/';
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

    // Drop names the listing no longer holds -- a file trashed or deleted
    // since the click. Nothing would show it, but the status bar would go on
    // counting it as selected.
    for (size_t i = 0; i < m_selected.size();) {
        bool found = false;
        for (const Entry& entry : m_entries) {
            if (entry.name == m_selected[i]) {
                found = true;
                break;
            }
        }
        if (found) {
            ++i;
        } else {
            m_selected.erase(m_selected.begin() + i);
        }
    }

    // Mark the column the list is ordered by, the way both browsers do.
    const char* kColumnNames[] = { "名称", "大小", "修改日期" };
    for (int i = 0; i < 3; ++i) {
        if (m_pColumnButtons[i] == nullptr) {
            continue;
        }
        U8String text(kColumnNames[i]);
        if (i == m_sortColumn) {
            text += m_sortAscending ? " ▲" : " ▼";
        }
        m_pColumnButtons[i]->SetText(text);
    }

    // The column headings only mean anything in the report view.
    if (m_pHeaderRow != nullptr) {
        m_pHeaderRow->SetVisible(!m_iconView);
    }

    // The selection is deliberately *not* cleared here. It is keyed by name,
    // so it survives the re-sort and the rebuild, and this is where a click's
    // new selection is asked to show up -- clearing it here is what made a
    // click on a row or a tile leave no highlight anywhere.
    m_pFileList->RemoveAllItems();

    if (m_iconView) {
        // A grid of tiles. The column count is taken from the pane's width
        // once, at build time: a resize re-lays the tiles but does not
        // re-flow them into a different number of columns.
        ui::UiRect client;
        GetClientRect(client);
        // Sidebar, the hairline beside it, the content's right inset, and the
        // handful of pixels the window's own edges take.
        int width = client.Width() - 198;
        // A tile takes its width plus the 4px margin on either side, and the
        // count has to allow for both: counting the tile alone fitted one
        // column too many, and the grid ran off the window's right edge.
        const int tileStep = kIconTileW + 8;
        if (width < tileStep) {
            width = tileStep;
        }
        const int columns = std::max(1, width / tileStep);

        ui::HBox* pRow = nullptr;
        for (size_t i = 0; i < m_entries.size(); ++i) {
            if (i % columns == 0) {
                pRow = new ui::HBox(this);
                pRow->SetAttribute("width", "stretch");
                pRow->SetAttribute("height", Num(kIconTileH));
                m_pFileList->AddItem(pRow);
            }
            const Entry& entry = m_entries[i];

            ui::ButtonVBox* pTile = new ui::ButtonVBox(this);
            pTile->SetAttribute("width", Num(kIconTileW));
            pTile->SetAttribute("height", Num(kIconTileH));
            pTile->SetAttribute("margin", "4,4,4,4");
            pTile->SetAttribute("cursor_type", "hand");
            // Selected and hot the same: the click that selects a tile leaves
            // it hot, and the two must not disagree (see the note on hover at
            // the top of this file).
            SetAllStateColors(pTile, IsSelected(i) ? kRowSelected : kTransparent);
            SetRadius(pTile, 8, true);

            ui::HBox* pIconCell = new ui::HBox(this);
            pIconCell->SetAttribute("width", Num(kIconTileW - 8));
            pIconCell->SetAttribute("height", "56");
            pIconCell->SetAttribute("halign", "center");
            // The cell centres its glyph: a box packs its children at the top
            // left unless it is told otherwise, so without this the icon sits
            // at the left of the cell while the name below it is centred, and
            // the two do not line up.
            pIconCell->SetAttribute("child_align", "hcenter,vcenter");
            pIconCell->SetMouseEnabled(false);
            pIconCell->AddItem(entry.isDir ? MakeFolderIcon(this, 42)
                                           : MakeFileIcon(this, 42));
            pTile->AddItem(pIconCell);

            ui::Label* pName = new ui::Label(this);
            pName->SetText(entry.name);
            pName->SetAttribute("font", "system_12");
            pName->SetStateTextColor(ui::kControlStateNormal,
                                     entry.isDir ? kAccent : kTextDark);
            pName->SetAttribute("text_align", "hcenter,vtop");
            pName->SetAttribute("width", Num(kIconTileW - 8));
            pName->SetAttribute("height", "34");
            pName->SetAttribute("halign", "center");
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
        SyncScrollRange(m_pFileList);
        UpdateStatus();
        return;
    }

    // Report view: icon, name, size, modified.
    for (size_t i = 0; i < m_entries.size(); ++i) {
        const Entry& entry = m_entries[i];
        ui::ButtonHBox* pRow = new ui::ButtonHBox(this);
        pRow->SetAttribute("height", Num(kRowHeight));
        pRow->SetAttribute("width", "stretch");
        pRow->SetAttribute("padding", "10,0,10,0");
        SetAllStateColors(pRow, IsSelected(i) ? kRowSelected : kTransparent);
        SetRadius(pRow, 6, true);
        pRow->SetAttribute("cursor_type", "hand");

        ui::HBox* pIconCell = new ui::HBox(this);
        pIconCell->SetAttribute("width", "32");
        pIconCell->SetAttribute("height", "stretch");
        pIconCell->SetAttribute("child_align", "hcenter,vcenter");
        pIconCell->SetMouseEnabled(false);
        pIconCell->AddItem(entry.isDir ? MakeFolderIcon(this, 20)
                                       : MakeFileIcon(this, 20));
        pRow->AddItem(pIconCell);

        ui::Label* pName = new ui::Label(this);
        pName->SetText(entry.name);
        pName->SetAttribute("font", "system_14");
        pName->SetStateTextColor(ui::kControlStateNormal,
                                 entry.isDir ? kAccent : kTextDark);
        pName->SetAttribute("text_align", "left,vcenter");
        pName->SetAttribute("width", "stretch");
        pName->SetAttribute("height", "stretch");
        pName->SetMouseEnabled(false);
        pRow->AddItem(pName);

        ui::Label* pSize = new ui::Label(this);
        pSize->SetText(entry.isDir ? "—" : FormatSize(entry.size));
        pSize->SetAttribute("font", "system_13");
        pSize->SetStateTextColor(ui::kControlStateNormal, kTextHint);
        pSize->SetAttribute("text_align", "right,vcenter");
        pSize->SetAttribute("width", "110");
        pSize->SetAttribute("height", "stretch");
        pSize->SetMouseEnabled(false);
        pRow->AddItem(pSize);

        ui::Label* pTime = new ui::Label(this);
        pTime->SetText(entry.mtime);
        pTime->SetAttribute("font", "system_13");
        pTime->SetStateTextColor(ui::kControlStateNormal, kTextHint);
        pTime->SetAttribute("text_align", "right,vcenter");
        pTime->SetAttribute("width", "160");
        pTime->SetAttribute("height", "stretch");
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

    SyncScrollRange(m_pFileList);
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
            // The anchor is a name, so it still marks the row it was clicked
            // on even if a reload has since sorted that row elsewhere.
            size_t anchor = index;
            for (size_t i = 0; i < m_entries.size(); ++i) {
                if (m_entries[i].name == m_anchor) {
                    anchor = i;
                    break;
                }
            }
            const size_t from = std::min(anchor, index);
            const size_t to = std::max(anchor, index);
            m_selected.clear();
            for (size_t i = from; i <= to; ++i) {
                m_selected.push_back(m_entries[i].name);
            }
        } else if (IsSelected(index)) {
            const U8String& name = m_entries[index].name;
            m_selected.erase(std::find(m_selected.begin(), m_selected.end(), name));
            m_anchor = name;
        } else {
            m_selected.push_back(m_entries[index].name);
            m_anchor = m_entries[index].name;
        }
    } else {
        m_selected.assign(1, m_entries[index].name);
        m_anchor = m_entries[index].name;
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
    m_selected.assign(1, m_entries[index].name);
    m_anchor = m_entries[index].name;

    m_pContextMenu->RemoveAllItems();
    const Entry& entry = m_entries[index];

    struct Action { const char* label; int id; };
    const Action actions[] = {
        { "打开", 0 },
        { "在终端中打开", 1 },
        { "新建文件夹", 2 },
        { "属性", 3 },
        { "移到废纸篓", 4 },
    };
    for (const Action& action : actions) {
        ui::Button* pItem = new ui::Button(this);
        pItem->SetText(U8String(action.label));
        pItem->SetAttribute("font", "system_14");
        pItem->SetStateTextColor(ui::kControlStateNormal,
                                 action.id == 4 ? "#FFFF453A" : kTextBody);
        pItem->SetAttribute("text_align", "left,vcenter");
        pItem->SetAttribute("text_padding", "10,0,10,0");
        pItem->SetAttribute("width", "stretch");
        pItem->SetAttribute("height", "30");
        pItem->SetAttribute("cursor_type", "hand");
        SetAllStateColors(pItem, kTransparent);
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
                U8String cmd = m_curDir;
                ShellQuote(cmd);
                LaunchCommand(U8String("wayst -e sh -c 'cd ") + cmd +
                              " && exec bash'");
            } else if (id == 2) {
                NewFolder();
            } else if (id == 3) {
                ShowProperties(row);
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
    const int height = 6 * 2 + 30 * 5;
    int px = pt.x;
    int py = pt.y;
    if (px + width > client.right) px = client.right - width - 4;
    if (py + height > client.bottom) py = client.bottom - height - 4;
    if (px < 4) px = 4;
    if (py < 4) py = 4;
    m_pContextMenu->SetAttribute("height", Num(height));
    m_pContextMenu->SetPos(ui::UiRect(px, py, px + width, py + height));
    m_pContextMenu->SetVisible(true);
    m_pContextMenu->Invalidate();
}

void FilesForm::ShowProperties(size_t index)
{
    if (m_pContextMenu == nullptr || index >= m_entries.size()) {
        return;
    }
    const Entry& entry = m_entries[index];

    U8String full = m_curDir;
    if (!full.empty() && full.back() != '/') {
        full += '/';
    }
    full += entry.name;

    std::string owner;
    struct stat st;
    if (stat(full.c_str(), &st) == 0) {
        char mode[16];
        std::snprintf(mode, sizeof(mode), "%o",
                      static_cast<unsigned>(st.st_mode & 07777));
        owner = mode;
    }

    m_pContextMenu->RemoveAllItems();
    ui::Label* pTitle = new ui::Label(this);
    pTitle->SetText(entry.name);
    pTitle->SetAttribute("font", "system_14");
    pTitle->SetStateTextColor(ui::kControlStateNormal, kTextDark);
    pTitle->SetAttribute("text_align", "left,vcenter");
    pTitle->SetAttribute("text_padding", "10,0,10,0");
    pTitle->SetAttribute("width", "stretch");
    pTitle->SetAttribute("height", "26");
    pTitle->SetMouseEnabled(false);
    m_pContextMenu->AddItem(pTitle);

    struct Row { const char* label; U8String value; };
    std::vector<Row> rows;
    rows.push_back({ "类型", entry.isDir ? U8String("文件夹") : U8String("文件") });
    if (!entry.isDir) {
        rows.push_back({ "大小", FormatSize(entry.size) });
    }
    rows.push_back({ "修改时间", entry.mtime });
    if (!owner.empty()) {
        rows.push_back({ "权限", U8String(owner.c_str()) });
    }
    rows.push_back({ "位置", m_curDir });

    for (const Row& row : rows) {
        ui::HBox* pLine = new ui::HBox(this);
        pLine->SetAttribute("width", "stretch");
        pLine->SetAttribute("height", "22");
        pLine->SetMouseEnabled(false);

        ui::Label* pKey = new ui::Label(this);
        pKey->SetText(U8String(row.label));
        pKey->SetAttribute("font", "system_12");
        pKey->SetStateTextColor(ui::kControlStateNormal, kTextHint);
        pKey->SetAttribute("text_align", "left,vcenter");
        pKey->SetAttribute("text_padding", "10,0,0,0");
        pKey->SetAttribute("width", "70");
        pKey->SetAttribute("height", "stretch");
        pKey->SetMouseEnabled(false);
        pLine->AddItem(pKey);

        ui::Label* pValue = new ui::Label(this);
        pValue->SetText(row.value);
        pValue->SetAttribute("font", "system_12");
        pValue->SetStateTextColor(ui::kControlStateNormal, kTextBody);
        pValue->SetAttribute("text_align", "left,vcenter");
        pValue->SetAttribute("width", "stretch");
        pValue->SetAttribute("height", "stretch");
        pValue->SetMouseEnabled(false);
        pLine->AddItem(pValue);

        m_pContextMenu->AddItem(pLine);
    }

    ui::UiRect client;
    GetClientRect(client);
    const int width = 300;
    const int height = 12 + 26 + static_cast<int>(rows.size()) * 22;
    int px = client.right - width - 40;
    int py = 140;
    if (px < 4) px = 4;
    if (py + height > client.bottom) py = client.bottom - height - 8;
    m_pContextMenu->SetAttribute("width", Num(width));
    m_pContextMenu->SetAttribute("height", Num(height));
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
    U8String source = m_curDir;
    if (!source.empty() && source.back() != '/') {
        source += '/';
    }
    source += m_entries[index].name;
    U8String trashDir(home);
    trashDir += "/.Trash";
    U8String target = trashDir + "/" + m_entries[index].name;

    U8String quotedSource = source;
    U8String quotedTarget = target;
    U8String quotedTrashDir = trashDir;
    ShellQuote(quotedSource);
    ShellQuote(quotedTarget);
    ShellQuote(quotedTrashDir);

    // Never clobber something already in the trash: a plain mv would.
    LaunchCommand(U8String("mkdir -p ") + quotedTrashDir + " && mv -n " +
                  quotedSource + " " + quotedTarget);
    Refresh();
}

void FilesForm::NewFolder()
{
    std::vector<Entry> existing;
    ListDirectory(m_curDir, existing);
    U8String name = "新建文件夹";
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
            U8String candidate = "新建文件夹 ";
            candidate += std::to_string(++suffix).c_str();
            name = candidate;
        }
    }

    U8String path = m_curDir;
    if (!path.empty() && path.back() != '/') {
        path += '/';
    }
    path += name;
    U8String quoted = path;
    ShellQuote(quoted);
    LaunchCommand(U8String("mkdir -p ") + quoted);
    Refresh();
}

void FilesForm::UpdateBreadcrumb()
{
    if (m_pBreadcrumb == nullptr) {
        return;
    }

    // Split the path into segments, each of which navigates to its prefix.
    std::vector<std::pair<U8String, U8String>> crumbs;   // label, full path
    crumbs.push_back({ "文件系统", U8String("/") });
    U8String accumulated("/");
    U8String rest = m_curDir;
    if (!rest.empty() && rest[0] == '/') {
        rest = rest.substr(1);
    }
    while (!rest.empty()) {
        const size_t slash = rest.find('/');
        const U8String segment = (slash == U8String::npos) ? rest : rest.substr(0, slash);
        if (!segment.empty()) {
            if (accumulated.size() > 1) {
                accumulated += '/';
            }
            accumulated += segment;
            crumbs.push_back({ segment, accumulated });
        }
        if (slash == U8String::npos) {
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
            const std::pair<U8String, U8String>& crumb = crumbs[first + i];
            U8String label = crumb.first;
            if (i == 0 && first > 0) {
                label = U8String("…/") + label;
            }
            pCrumb->SetText(label);
            m_crumbTargets[i] = crumb.second;
            const bool last = (static_cast<size_t>(i) + 1 == shown);
            pCrumb->SetStateTextColor(ui::kControlStateNormal,
                                      last ? kTextDark : kTextHint);
        } else {
            // Slots past the end of the path are emptied, separator and all.
            // They keep their width -- dui lays a control out once -- so the
            // only thing that can make them disappear is painting nothing.
            pCrumb->SetText("");
            m_crumbTargets[i] = "";
        }
    }

    // One separator per gap between the segments that are actually shown, and
    // none after the last one: m_pCrumbSeps[i-1] belongs in front of slot i.
    for (size_t i = 0; i < m_pCrumbSeps.size(); ++i) {
        const bool wanted = (i + 1) < shown;
        m_pCrumbSeps[i]->SetText(wanted ? "›" : "");
    }

    // Highlight the deepest place the current directory sits under. Only one
    // of them: "/" is a prefix of everything, and /home/shxu is a prefix of
    // the Desktop, so a plain prefix test lights half the sidebar.
    // The deepest place whose path is a prefix of the current directory -- and
    // the path itself is what gets compared afterwards, not its length. Two
    // places can be the same length: /home/shxu/Documents and
    // /home/shxu/Downloads are both 21 characters, so a "length == the deepest
    // match's length" test lit both of them whenever the window was in either.
    const U8String* best = nullptr;
    if (!m_curDir.empty()) {
        for (const U8String& target : m_placePaths) {
            if (target.empty() || target.size() > m_curDir.size()) {
                continue;
            }
            if (m_curDir.compare(0, target.size(), target) != 0) {
                continue;
            }
            const bool atBoundary = target.size() == m_curDir.size() ||
                (target != "/" && m_curDir[target.size()] == '/');
            if (!atBoundary) {
                continue;
            }
            if (best == nullptr || target.size() > best->size()) {
                best = &target;
            }
        }
    }
    for (size_t i = 0; i < m_pPlaceButtons.size(); ++i) {
        const bool here = best != nullptr && i < m_placePaths.size() &&
            m_placePaths[i] == *best;
        // Every state, and all of them cleared again when the window moves on:
        // the highlight of the place we are at is the only one there should
        // ever be, and a control left in some other state must not show it.
        SetAllStateColors(m_pPlaceButtons[i], here ? kRowSelected : kTransparent);
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
        m_pStatusLabel->SetText(m_curDir);
    }
    if (m_pSelectionLabel != nullptr) {
        // The full path sits here rather than in a floating label over the
        // breadcrumb: a float reports whatever size dui estimates for it and
        // ignores the rectangle it is given, so it could never span the row.
        // The status bar has shown the path in this browser's ancestors since
        // Explorer 95, and it needs no dynamic sizing at all.
        size_t dirs = 0;
        for (const Entry& entry : m_entries) {
            if (entry.isDir) {
                ++dirs;
            }
        }
        U8String text = Num(static_cast<int>(m_entries.size())) + " 个项目";
        if (dirs > 0) {
            text += "（" + Num(static_cast<int>(dirs)) + " 个文件夹）";
        }
        if (!m_selected.empty()) {
            text += "  已选 " + Num(static_cast<int>(m_selected.size())) + " 项";
        }
        m_pSelectionLabel->SetText(text);
    }
}

void FilesForm::ShellQuote(U8String& arg)
{
    U8String quoted = "'";
    for (U8String::value_type ch : arg) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted += "'";
    arg = quoted;
}

void FilesForm::OpenEntry(const Entry& entry)
{
    U8String full = m_curDir;
    if (!full.empty() && full.back() != '/') {
        full += '/';
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
        U8String cmd = full;
        ShellQuote(cmd);
        LaunchCommand(cmd);
        return;
    }

    U8String quoted = full;
    ShellQuote(quoted);
    LaunchCommand(U8String("wayst -e vim ") + quoted);
}

void FilesForm::LaunchCommand(const U8String& cmdline)
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
