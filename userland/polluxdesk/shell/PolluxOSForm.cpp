#include "PolluxOSForm.h"
#include "PolluxPaths.h"
#include "WifiData.h"

#include <algorithm>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <string>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#include <unistd.h>

namespace {

// ---------------------------------------------------------------------------
// Appearance palette.
//
// These were compile-time constants. They are now filled in by
// ApplyAppearance() before each build so the shell can follow the light/dark
// setting and the accent colour -- dui has no runtime theme switching, so the
// shell carries its own small palette instead of relying on the theme layer
// (and the macos26 theme in particular would drop the CJK font fallback and
// turn every Chinese label into boxes).
//
// Colours are #AARRGGBB. The translucent ones let the wallpaper show through
// so the bar and dock read as frosted glass rather than flat grey.
// ---------------------------------------------------------------------------
struct Palette
{
    U8String barBg;          // frosted menu bar
    U8String barBorder;      // hairline under the menu bar
    U8String barHot;         // hovered menu-bar button
    U8String menuPanelBg;    // frosted dropdown panel
    U8String menuPanelLine;
    U8String menuItemHot;
    U8String dockBorder;
    U8String dockSeparator;  // hairline before the trash
    U8String dockDot;        // running-application indicator
    U8String chipBg;         // minimized-window chip
    U8String chipBorder;
    U8String chipHot;
    U8String textDark;
    U8String textBody;
    U8String textHint;
    U8String accent;
    U8String danger;
    U8String transparent;
    // Which status-glyph set to load. dui cannot tint an SVG, so each glyph
    // ships in a dark and a light version and the shell picks one.
    U8String glyphVariant;
    U8String dockCanBody;    // trash-can fill, translucent like the reference
    U8String dockCanEdge;
};

Palette g_pal;

// The accent at a lower alpha, for hover and selection washes. The settings
// module hands the accent over as "#AARRGGBB", so only the alpha pair moves.
U8String AccentWash(const U8String& accent, const char* alpha)
{
    if (accent.size() != 9 || accent[0] != '#') {
        return accent;
    }
    return U8String("#") + U8String(alpha) + accent.substr(3);
}

void BuildPalette(const pollux::Settings& settings)
{
    const bool dark = pollux::IsDark(settings);
    const U8String accent(pollux::AccentHex(settings));

    g_pal.transparent  = "#00000000";
    g_pal.accent       = accent;
    g_pal.glyphVariant = dark ? "white" : "black";
    // A light can would vanish on the light dock and a dark one on the dark
    // dock, so the can follows the appearance the way macOS's does.
    g_pal.dockCanBody  = dark ? "#D9F2F2F5" : "#E6F7F7FA";
    g_pal.dockCanEdge  = dark ? "#8CFFFFFF" : "#5E000000";
    g_pal.danger       = "#FFFF453A";
    // Same accent, washed out -- one source of truth for the highlight colour.
    g_pal.barHot       = AccentWash(accent, "22");
    g_pal.menuItemHot  = AccentWash(accent, "33");
    g_pal.chipHot      = AccentWash(accent, "33");

    if (dark) {
        g_pal.barBg        = "#D91C1C1E";
        g_pal.barBorder    = "#26FFFFFF";
        g_pal.menuPanelBg  = "#F21C1C1E";
        g_pal.menuPanelLine= "#26FFFFFF";
        g_pal.dockBorder   = "#66FFFFFF";
        g_pal.dockSeparator= "#59FFFFFF";
        g_pal.dockDot      = "#B3FFFFFF";
        g_pal.chipBg       = "#26FFFFFF";
        g_pal.chipBorder   = "#33FFFFFF";
        g_pal.textDark     = "#FFF5F5F7";
        g_pal.textBody     = "#FFD8D8DC";
        g_pal.textHint     = "#FF98989D";
    } else {
        g_pal.barBg        = "#F2FFFFFF";
        g_pal.barBorder    = "#33000000";
        g_pal.menuPanelBg  = "#F5FFFFFF";
        g_pal.menuPanelLine= "#33000000";
        g_pal.dockBorder   = "#A0C7E8FF";
        g_pal.dockSeparator= "#3D000000";
        g_pal.dockDot      = "#99000000";
        g_pal.chipBg       = "#1A000000";
        g_pal.chipBorder   = "#26000000";
        g_pal.textDark     = "#FF1D1D1F";
        g_pal.textBody     = "#FF3A3A3C";
        g_pal.textHint     = "#FF8E8E93";
    }
}

// Number of fixed menu buttons: 文件 / 编辑 / 显示 / 前往 / 窗口 / 帮助.
const int kMenuButtonCount = 6;
const U8String kMenuButtonText[kMenuButtonCount] = {
    "文件", "编辑", "显示", "前往", "窗口", "帮助"
};

// Layout attributes are strings; this keeps the derived sizes readable.
static U8String Num(int value)
{
    return ui::StringUtil::Printf("%d", value);
}

const int kPopoverWidth = 300;
const int kPopoverPad   = 10;
const int kPopoverRowH  = 26;

// "left,top,right,bottom" with equal horizontal margins.
static U8String MarginH(int px)
{
    return ui::StringUtil::Printf("%d,0,%d,0", px, px);
}

} // namespace

// ---------------------------------------------------------------------------
// Menu bar entries. Disabled items (cmd == nullptr, enabled == false) are
// rendered grayed-out like their macOS counterparts; separators are hairlines.
// ---------------------------------------------------------------------------
const PolluxOSForm::MenuItem PolluxOSForm::kAppMenu[] = {
    { "关于 PolluxOS",  "wayst -e sh -c 'echo \"PolluxOS (dui shell + wlroots compositor)\"; echo \"FreeBSD / Wayland / macOS style\"; read _'", false, true },
    { "系统设置",       "\"$HOME/projects-main/polluxos/build/polluxdesk/bin/polluxdesk_settings\"", false, true },
    { "键盘快捷键",     nullptr, false, false },
    { nullptr,          nullptr, true, false },
    { "锁定屏幕",       nullptr, false, false },
    { "退出登录",       "--logout", false, true },
    { "重新启动",       "shutdown -r now", false, true },
    { "关机",           "shutdown -p now", false, true },
};

const PolluxOSForm::MenuItem PolluxOSForm::kFileMenu[] = {
    { "新建终端",       "wayst", false, true },
    { "新建编辑器",     "wayst -e vim", false, true },
    { "文件管理器",     "\"$HOME/projects-main/polluxos/build/polluxdesk/bin/polluxdesk_files\" 2>/dev/null || wayst -e sh -c 'echo 未安装 polluxdesk_files; read _'", false, true },
    { "主目录",         "wayst -e sh -c 'cd \"$HOME\" && exec bash'", false, true },
    { "项目目录",       "wayst -e sh -c 'cd \"$HOME/projects-main/polluxos\" && exec bash'", false, true },
    { nullptr,          nullptr, true, false },
    { "关闭窗口",       nullptr, false, false },
};

const PolluxOSForm::MenuItem PolluxOSForm::kEditMenu[] = {
    { "剪切",           nullptr, false, false },
    { "复制",           nullptr, false, false },
    { "粘贴",           nullptr, false, false },
    { nullptr,          nullptr, true, false },
    { "全选",           nullptr, false, false },
};

const PolluxOSForm::MenuItem PolluxOSForm::kViewMenu[] = {
    /* Window geometry is now handled by the compositor titlebar buttons
     * (green = fullscreen, titlebar drag = move). */
    { "切换全屏",       nullptr, false, false },
    { "切换浮动窗口",   nullptr, false, false },
    { "水平分屏",       nullptr, false, false },
    { "垂直分屏",       nullptr, false, false },
};

const PolluxOSForm::MenuItem PolluxOSForm::kGoMenu[] = {
    { "主目录",         "wayst -e sh -c 'cd \"$HOME\" && exec bash'", false, true },
    { "项目目录",       "wayst -e sh -c 'cd \"$HOME/projects-main/polluxos\" && exec bash'", false, true },
};

const PolluxOSForm::MenuItem PolluxOSForm::kWindowMenu[] = {
    { "最小化",         nullptr, false, false },
    { "最大化",         nullptr, false, false },
    { "关闭窗口",       nullptr, false, false },
};

const PolluxOSForm::MenuItem PolluxOSForm::kHelpMenu[] = {
    { "PolluxOS 说明",  "wayst -e sh -c 'cat \"$HOME/projects-main/dui/README.md\"; echo; echo \"按回车关闭\"; read _'", false, true },
    { "dui 文档",       "wayst -e sh -c 'cat \"$HOME/projects-main/dui/docs/Summary.md\"; echo; echo \"按回车关闭\"; read _'", false, true },
};

const PolluxOSForm::MenuItem* PolluxOSForm::kMenuBarMenus[] = {
    kAppMenu, kFileMenu, kEditMenu, kViewMenu, kGoMenu, kWindowMenu, kHelpMenu,
};

const int PolluxOSForm::kMenuBarMenuCounts[] = {
    static_cast<int>(sizeof(kAppMenu) / sizeof(kAppMenu[0])),
    static_cast<int>(sizeof(kFileMenu) / sizeof(kFileMenu[0])),
    static_cast<int>(sizeof(kEditMenu) / sizeof(kEditMenu[0])),
    static_cast<int>(sizeof(kViewMenu) / sizeof(kViewMenu[0])),
    static_cast<int>(sizeof(kGoMenu) / sizeof(kGoMenu[0])),
    static_cast<int>(sizeof(kWindowMenu) / sizeof(kWindowMenu[0])),
    static_cast<int>(sizeof(kHelpMenu) / sizeof(kHelpMenu[0])),
};

const PolluxOSForm::MenuItem PolluxOSForm::kQuickMenu[] = {
    { "新建终端",       "wayst", false, true },
    { "应用菜单",       "wayst -e sh -c 'ls /usr/local/share/applications \"$HOME/.local/share/applications\" 2>/dev/null | sed s/.desktop// | head -40; read _'", false, true },
    { "系统设置",       "\"$HOME/projects-main/polluxos/build/polluxdesk/bin/polluxdesk_settings\"", false, true },
    { nullptr,          nullptr, true, false },
    { "退出登录",       "--logout", false, true },
};

const int PolluxOSForm::kQuickMenuCount =
    static_cast<int>(sizeof(kQuickMenu) / sizeof(kQuickMenu[0]));

// Dock launchers: macOS-style colored app tiles (wayst / vim available
// on the FreeBSD test host); everything is launched with fork/exec via /bin/sh.
// Like macOS, the dock shows icons only - names are tooltips.
// An empty `cmd` is the Launchpad tile: clicking opens the in-shell app grid.
const PolluxOSForm::DockApp PolluxOSForm::kDockApps[] = {
    { "终端",   ">_",  "polluxdesk/icons/terminal.svg", "#FF4C9FDB", "#FF2E6FA3", "wayst", "wayst", 0 },
    { "启动台", "⊞",   "polluxdesk/icons/apps.svg", "#FF8E7CC3", "#FF5F4B8B", "", "", 0 },
    { "文件",   "~",   "polluxdesk/icons/files.svg", "#FFF0A35C", "#FFC97A2B", "\"$HOME/projects-main/polluxos/build/polluxdesk/bin/polluxdesk_files\" 2>/dev/null || wayst -e sh -c 'echo 未安装 polluxdesk_files; read _'", "polluxdesk_files", 0 },
    { "浏览器", "@",   "polluxdesk/icons/browser.svg", "#FF5AA9E6", "#FF2F6FAB", "wayst -e sh -c 'firefox 2>/dev/null || chromium 2>/dev/null || (echo \"未安装浏览器\"; sleep 2)'", "firefox|chromium|chrome", 0 },
    { "设置",   "*",   "polluxdesk/icons/settings.svg", "#FF9AA4B0", "#FF6B7580", "\"$HOME/projects-main/polluxos/build/polluxdesk/bin/polluxdesk_settings\"", "polluxdesk_settings", 1 },
};

// Icons for programs that are running without a pinned tile -- the Wi-Fi
// window and Activity among them. Same embedded SVGs the launchpad uses; the
// fallback is the generic one, so a program nobody thought of still gets a
// tile rather than a gap in the dock.
struct RunningIcon
{
    const char* exe;
    const char* icon;
    const char* color;
    const char* color2;
};
const RunningIcon kRunningIcons[] = {
    { "polluxdesk_wifi",     "polluxdesk/icons/wifi.svg",     "#FF5AA9E6", "#FF2F6FAB" },
    { "polluxdesk_activity", "polluxdesk/icons/activity.svg", "#FF8E7CC3", "#FF5F4B8B" },
    { "polluxdesk_settings", "polluxdesk/icons/settings.svg", "#FF9AA4B0", "#FF6B7580" },
    { "polluxdesk_files",    "polluxdesk/icons/files.svg",    "#FFF0A35C", "#FFC97A2B" },
    { "polluxdesk_apps",     "polluxdesk/icons/apps.svg",     "#FF8E7CC3", "#FF5F4B8B" },
    { "wayst",               "polluxdesk/icons/terminal.svg", "#FF4C9FDB", "#FF2E6FA3" },
    { "vim",                 "polluxdesk/icons/editor.svg",   "#FF7FB069", "#FF4E7A3F" },
};
const int kRunningIconCount =
    static_cast<int>(sizeof(kRunningIcons) / sizeof(kRunningIcons[0]));

PolluxOSForm::PolluxOSForm(bool dockOverlay, PolluxOSForm* desktopOwner,
                           bool menuOverlay)
    : m_dockOverlay(dockOverlay), m_desktopOwner(desktopOwner),
      m_menuOverlay(menuOverlay)
{
    // Dock-launched apps are forked and never waited on; ignore SIGCHLD so
    // exited children are auto-reaped instead of accumulating as zombies.
    signal(SIGCHLD, SIG_IGN);
}

PolluxOSForm::~PolluxOSForm()
{
    if (m_clockTimerId > 0) {
        ui::GlobalManager::Instance().Timer().RemoveTimer(m_clockTimerId);
        m_clockTimerId = 0;
    }
}

U8String PolluxOSForm::GetSkinFolder()
{
    return "";
}

U8String PolluxOSForm::GetSkinFile()
{
    // Pure code mode: no layout XML is loaded
    return "";
}

void PolluxOSForm::SetCompositorMenuOpen(bool open, int menuHeight)
{
    if (m_dockOverlay) {
        m_dockMenuOpen = open;
        SetDockCompositorTitle();
    } else if (m_menuOverlay) {
        if (open) {
            SetText("PolluxOS MenuBar (menu:" + Num(menuHeight) + ")");
        } else {
            SetText("PolluxOS MenuBar");
        }
    } else {
        SetText(open ? "PolluxOS Desktop (menu)" : "PolluxOS Desktop");
    }
}

void PolluxOSForm::SetDockCompositorTitle()
{
    if (!m_dockOverlay) {
        return;
    }
    if (m_dockHitAreaValid) {
        SetText(ui::StringUtil::Printf(
            "PolluxOS Dock (area:%d,%d,%d,%d,%d;menu:%d)",
            m_dockHitArea.left, m_dockHitArea.top,
            m_dockHitArea.Width(), m_dockHitArea.Height(), m_dockHitRadius,
            m_dockMenuOpen ? 1 : 0));
    } else {
        SetText(m_dockMenuOpen ? "PolluxOS Dock (menu)" : "PolluxOS Dock");
    }
}

void PolluxOSForm::UpdateDockHitArea()
{
    if (!m_dockOverlay || m_pDockBar == nullptr) {
        return;
    }
    // GetRect() is in window-client coordinates, which for the fullscreen
    // dock toplevel are also compositor output coordinates. Restrict the
    // dock's hit region to its actual frosted bar, not the full bottom band.
    const ui::UiRect rc = m_pDockBar->GetRect();
    if (rc.IsEmpty()) {
        return;
    }
    const int iconPx = pollux::DockIconPx(m_settings);
    const int cornerRadius = iconPx * 18 / 56;
    if (m_dockHitAreaValid && rc.left == m_dockHitArea.left &&
            rc.top == m_dockHitArea.top && rc.right == m_dockHitArea.right &&
            rc.bottom == m_dockHitArea.bottom &&
            cornerRadius == m_dockHitRadius) {
        // The layout may have repositioned floating run indicators without
        // changing the dock's outer rectangle; align them below each icon too.
    } else {
        m_dockHitArea = rc;
        m_dockHitRadius = cornerRadius;
        m_dockHitAreaValid = true;
        SetDockCompositorTitle();
    }
    const int dotSize = std::max(3, iconPx / 14);
    for (size_t i = 0; i < m_dockDots.size() &&
            i < m_dockDotAnchors.size(); ++i) {
        ui::Control* dot = m_dockDots[i];
        ui::Control* anchor = m_dockDotAnchors[i];
        if (dot == nullptr || anchor == nullptr || anchor->GetRect().IsEmpty()) {
            continue;
        }
        const ui::UiRect icon = anchor->GetRect();
        const int x = (icon.left + icon.right - dotSize) / 2;
        const int y = m_dockHitArea.bottom - 6 + (6 - dotSize) / 2;
        const ui::UiRect current = dot->GetRect();
        if (current.left != x || current.top != y ||
                current.right != x + dotSize || current.bottom != y + dotSize) {
            dot->SetPos(ui::UiRect(x, y, x + dotSize, y + dotSize));
        }
    }
}

void PolluxOSForm::RequestRestoreWindow(unsigned long id)
{
    PolluxOSForm* target = (m_dockOverlay || m_menuOverlay) &&
                           m_desktopOwner != nullptr
                               ? m_desktopOwner
                               : this;
    target->SetText(ui::StringUtil::Printf(
        "PolluxOS Desktop (restore:%lu)", id));
    target->m_titleMarkerPending = true;
}

void PolluxOSForm::GetCreateWindowAttributes(ui::WindowCreateAttributes& attrs)
{
    attrs.m_bInitSizeDefined = true;
    attrs.m_szInitSize.cx = 1280;
    attrs.m_szInitSize.cy = m_menuOverlay ? 30 : 800;
    attrs.m_bShadowAttached = false;
    attrs.m_bShadowAttachedDefined = true;
    attrs.m_bIsLayeredWindow = true;
    attrs.m_bIsLayeredWindowDefined = true;
    // No caption / resize border: a bare fullscreen desktop shell.
    attrs.m_rcCaption = ui::UiRect(0, 0, 0, 0);
    attrs.m_bCaptionDefined = true;
    attrs.m_rcSizeBox = ui::UiRect(0, 0, 0, 0);
    attrs.m_bSizeBoxDefined = true;
    BaseClass::GetCreateWindowAttributes(attrs);
}

void PolluxOSForm::OnInitWindow()
{
    // The wlroots compositor owns every shadow and titlebar; the shell
    // surface itself must stay borderless and shadowless.
    SetShadowAttached(false);
    // Thumbnails are named after window pointers, so a stale one from an
    // earlier session could be shown against an unrelated window. Start
    // clean; live windows re-grab as they are minimized.
    if (!m_dockOverlay && !m_menuOverlay) {
        const std::string thumbClean =
            "rm -f '" + pollux::ThumbDir() + "'/*.png 2>/dev/null";
        pollux::Run(thumbClean.c_str());
    }
    m_settings = pollux::Load();
    m_settingsMtime = pollux::MtimeNs(pollux::ConfigPath());
    ApplyAppearance();
    BuildUi();

    BaseClass::OnInitWindow();

    // The shell fills the workspace as the single tiled window; apps launched
    // from the dock float above it (see the compositor titlebar management).
    // Deliberately NOT fullscreen: a fullscreen window would cover the apps.
    StartClock();
}

void PolluxOSForm::LaunchApp(const char* cmdline)
{
    if (cmdline == nullptr || cmdline[0] == '\0') {
        return;
    }
    printf("[polluxdesk] launch: %s\n", cmdline);
    fflush(stdout);

    // Logout swaps the shell client under the SAME compositor: start the
    // greeter first, then close this window. The greeter wrapper switches to
    // the new client, so the DRM output is never torn down (no flicker).
    // "--logout" is a sentinel, not a program: the launcher swap below is the
    // whole logout, so there is no script to run for it.
    if (std::strcmp(cmdline, "--logout") == 0) {
        const char* home = std::getenv("HOME");
        U8String launcher = U8String(home != nullptr ? home : "/home/shxu") +
                           "/projects-main/polluxos/build/polluxdesk/bin/launcher";
        pid_t pid = fork();
        if (pid < 0) {
            perror("[polluxdesk] logout fork");
            return;
        }
        if (pid == 0) {
            setsid();
            execl(launcher.c_str(), "launcher", static_cast<char*>(nullptr));
            perror("[polluxdesk] logout exec launcher");
            _exit(127);
        }
        CloseWnd(ui::kWindowCloseNormal);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("[polluxdesk] fork");
        return;
    }
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmdline, static_cast<char*>(nullptr));
        perror("[polluxdesk] execl");
        _exit(127);
    }
}

// ---------------------------------------------------------------------------
// Launchpad: a separate always-on-top overlay window (LaunchPadForm) titled
// "PolluxOS Launchpad". The compositor keeps that title centered, borderless
// and pinned above regular app windows, so the grid is never buried under
// terminals like the old in-shell panel was.
// ---------------------------------------------------------------------------

#include "LaunchPadForm.h"

void PolluxOSForm::ShowLaunchPad()
{
    // Run Apps as a separate Wayland client. dui's input routing is reliable
    // for one window per process; the old second-window implementation made
    // the panel's clicks, wheel and keyboard events disappear.
    LaunchApp("\"" POLLUX_BIN "/polluxdesk_apps\"");
}

void PolluxOSForm::HideAppPanel()
{
    if (!m_launchPadWeak.expired() && m_pLaunchPad != nullptr) {
        m_pLaunchPad->HidePad();
    }
}

bool PolluxOSForm::IsAppPanelVisible() const
{
    return !m_launchPadWeak.expired() && m_pLaunchPad->IsWindowVisible();
}

void PolluxOSForm::SetMenuButtonActive(int index, bool active)
{
    if (index < 0 || index >= kMenuButtonCount || m_menuButtons[index] == nullptr) {
        return;
    }
    m_menuButtons[index]->SetStateColor(ui::kControlStateNormal,
                                        active ? g_pal.barHot : g_pal.transparent);
}

void PolluxOSForm::HideMenuPanel()
{
    // Any menu interaction replaces the Launchpad overlay as well.
    HideAppPanel();
    if (m_pMenuPanel != nullptr) {
        // Capture the panel rect before hiding it. On the Wayland backend,
        // simply SetVisible(false) may not damage the previously painted
        // dropdown area; invalidating the full client guarantees the wallpaper /
        // desktop underneath is repainted and the menu does not linger on
        // screen after the pointer moves away or the user clicks elsewhere.
        const ui::UiRect rcMenu = m_pMenuPanel->GetPos();
        m_pMenuPanel->SetVisible(false);
        if (!rcMenu.IsEmpty()) {
            Invalidate(rcMenu);
        }
        InvalidateAll();
    }
    SetMenuButtonActive(m_openMenuIndex, false);
    m_openMenuIndex = -1;
    // Notify the compositor that the desktop dropdown is closed; it lowers the
    // shell back behind app windows and restores normal app interaction.
    SetCompositorMenuOpen(false);
}

void PolluxOSForm::ToggleMenu(int menuIndex)
{
    if (menuIndex < 0 || menuIndex >= kMenuButtonCount) {
        return;
    }
    if (m_openMenuIndex == menuIndex && m_pMenuPanel != nullptr && m_pMenuPanel->IsVisible()) {
        HideMenuPanel();
        return;
    }

    HideMenuPanel();

    ui::UiRect rc = m_menuButtons[menuIndex]->GetRect();
    if (rc.IsEmpty()) {
        // Layout not finished yet; fall back to a position under the bar.
        rc = ui::UiRect(120 + menuIndex * 64, 30, 120 + menuIndex * 64, 30);
    }
    ShowMenuPanel(kMenuBarMenus[menuIndex + 1], kMenuBarMenuCounts[menuIndex + 1],
                  rc.left, rc.bottom + 2);

    m_openMenuIndex = menuIndex;
    SetMenuButtonActive(menuIndex, true);
}

void PolluxOSForm::ShowMenuPanel(const MenuItem* items, int count, int x, int y)
{
    if (items == nullptr || count <= 0 || m_pMenuPanel == nullptr) {
        return;
    }

    m_pMenuPanel->RemoveAllItems();

    const int kItemHeight = 30;
    const int kPanelPadding = 6;
    int panelHeight = kPanelPadding * 2;

    for (int i = 0; i < count; ++i) {
        const MenuItem& entry = items[i];
        if (entry.separator) {
            panelHeight += 9;   // 1px hairline + 4px vertical margins
            ui::Control* pSep = new ui::Control(this);
            pSep->SetAttribute("height", "1");
            pSep->SetAttribute("width", "stretch");
            pSep->SetAttribute("margin", "8,4,8,4");
            pSep->SetBkColor(g_pal.menuPanelLine);
            pSep->SetMouseEnabled(false);
            m_pMenuPanel->AddItem(pSep);
            continue;
        }

        panelHeight += kItemHeight;
        ui::Button* pItem = new ui::Button(this);
        pItem->SetText(U8String(entry.text));
        pItem->SetAttribute("font", "system_14");
        pItem->SetAttribute("text_align", "left,vcenter");
        pItem->SetAttribute("text_padding", "10,0,10,0");
        pItem->SetAttribute("height", "30");
        pItem->SetAttribute("width", "218");
        pItem->SetAttribute("margin", "0,0,0,0");
        pItem->SetStateColor(ui::kControlStateNormal, g_pal.transparent);
        pItem->SetStateColor(ui::kControlStateHot,
                             entry.enabled ? g_pal.menuItemHot : g_pal.transparent);
        pItem->SetStateColorRound(ui::kControlStateNormal, ui::UiSize(6, 6), false);
        pItem->SetStateColorRound(ui::kControlStateHot, ui::UiSize(6, 6), false);
        pItem->SetEnabled(entry.enabled);
        // SetStateTextColor, not the "text_color" attribute: dui resolves a
        // label's colour from its per-state text colour map and falls back to
        // the *global* default, and no label control implements a text_color
        // attribute at all -- setting one is silently ignored. That went
        // unnoticed while the shell was light, because dui's default is black
        // and black on white looks deliberate. Disabled items also get their
        // state set, or they would fall back to the global disabled colour.
        pItem->SetStateTextColor(ui::kControlStateNormal,
                            entry.enabled ? g_pal.textBody : g_pal.textHint);
        pItem->SetStateTextColor(ui::kControlStateDisabled, g_pal.textHint);
        pItem->SetAttribute("cursor_type", entry.enabled ? "hand" : "arrow");

        const char* cmd = entry.cmd;
        pItem->AttachClick([this, cmd](const ui::EventArgs& /*args*/) {
            HideMenuPanel();
            if (cmd != nullptr && cmd[0] != '\0') {
                LaunchApp(cmd);
            }
            return true;
        });
        m_pMenuPanel->AddItem(pItem);
    }

    m_pMenuPanel->SetAttribute("height",
                               ui::StringUtil::Printf("%d", panelHeight));

    ui::UiRect client;
    GetClientRect(client);
    const int kPanelWidth = 230;
    int px = x;
    int py = y;
    if (px + kPanelWidth > client.right) {
        px = client.right - kPanelWidth - 8;
    }
    if (py + panelHeight > client.bottom) {
        py = client.bottom - panelHeight - 8;
    }
    if (px < 4) {
        px = 4;
    }
    if (py < 30) {
        py = 30;
    }

    m_pMenuPanel->SetPos(ui::UiRect(px, py, px + kPanelWidth, py + panelHeight));
    m_pMenuPanel->SetPaintOrder(100);   // paint above bar / dock / desktop
    m_pMenuPanel->SetVisible(true);
    // Signal the compositor that a desktop dropdown is open so it can raise
    // the shell above app windows for the duration of the menu.
    SetCompositorMenuOpen(true, py + panelHeight + 8);
    Invalidate(m_pMenuPanel->GetPos());
}

void PolluxOSForm::ShowQuickMenu(int x, int y)
{
    // Opening the right-click menu replaces any open dropdown; clear the
    // previous menu-button highlight first.
    HideMenuPanel();
    ShowMenuPanel(kQuickMenu, kQuickMenuCount, x, y);
}

void PolluxOSForm::AttachMenuDismissHandlers()
{
    // Moving the pointer out of the menu bar and the open dropdown dismisses
    // the menu (macOS-style "mouse away" behavior).
    AttachWindowMouseMoveMsg([this](const ui::EventArgs& args) {
        if (m_pMenuPanel == nullptr || !m_pMenuPanel->IsVisible()) {
            return true;
        }

        // macOS menu tracking: after the first click opens a menu, hovering
        // another menu title switches to that menu without another click.
        if (m_pAppButton != nullptr &&
            m_pAppButton->GetPos().ContainsPt(args.ptMouse)) {
            if (m_openMenuIndex != -1) {
                HideMenuPanel();
                ui::UiRect rc = m_pAppButton->GetRect();
                ShowMenuPanel(kAppMenu, kMenuBarMenuCounts[0],
                              rc.IsEmpty() ? 12 : rc.left, rc.bottom + 2);
                m_openMenuIndex = -1;
            }
            return true;
        }
        for (int mi = 0; mi < kMenuButtonCount; ++mi) {
            if (m_menuButtons[mi] != nullptr &&
                m_menuButtons[mi]->GetPos().ContainsPt(args.ptMouse)) {
                if (m_openMenuIndex != mi) {
                    ToggleMenu(mi);
                }
                return true;
            }
        }

        if (!ShouldMenuStayOpenForMove(args.ptMouse)) {
            HideMenuPanel();
        }
        return true;
    });

    // Pressing anywhere outside the dropdown and the menu-bar buttons closes
    // it too. The desktop already does this, but this also covers the empty
    // menu-bar area and the dock row. (The Launchpad is its own overlay
    // window now and dismisses itself on focus loss.)
    AttachWindowLButtonDownMsg([this](const ui::EventArgs& args) {
        if (m_pMenuPanel != nullptr && m_pMenuPanel->IsVisible() &&
            !ShouldMenuStayOpenForPress(args.ptMouse)) {
            HideMenuPanel();
        }
        return true;
    });

    // If the shell is ever run in a smaller/nested window, leaving the window
    // should also close the menus.
    AttachWindowMouseLeaveMsg([this](const ui::EventArgs& /*args*/) {
        HideMenuPanel();
        HideAppPanel();
        return true;
    });
}

bool PolluxOSForm::IsPointInMenuButton(const ui::UiPoint& pt) const
{
    if (m_pAppButton != nullptr && m_pAppButton->GetPos().ContainsPt(pt)) {
        return true;
    }
    for (int i = 0; i < kMenuButtonCount; ++i) {
        if (m_menuButtons[i] != nullptr && m_menuButtons[i]->GetPos().ContainsPt(pt)) {
            return true;
        }
    }
    return false;
}

bool PolluxOSForm::ShouldMenuStayOpenForMove(const ui::UiPoint& pt) const
{
    if (m_pMenuPanel == nullptr || !m_pMenuPanel->IsVisible()) {
        return true;
    }
    // Moving along the top bar (between menu buttons) should not close the
    // dropdown; only leaving both the bar and the panel closes it.
    if (m_pMenuBar != nullptr && m_pMenuBar->GetPos().ContainsPt(pt)) {
        return true;
    }
    const ui::UiRect panelRect = m_pMenuPanel->GetPos();
    if (panelRect.ContainsPt(pt)) {
        return true;
    }
    // Keep the small vertical gap between the menu bar and the dropdown part
    // of the same "menu zone", so moving from a menu button into the panel
    // (or back) does not dismiss it while crossing that 1-2px gap.
    if (m_pMenuBar != nullptr) {
        const ui::UiRect barRect = m_pMenuBar->GetPos();
        if (pt.y >= barRect.bottom && pt.y <= panelRect.top) {
            ui::UiRect bridge(panelRect.left, barRect.bottom,
                              panelRect.right, panelRect.top);
            if (bridge.ContainsPt(pt)) {
                return true;
            }
        }
    }
    return false;
}

bool PolluxOSForm::ShouldMenuStayOpenForPress(const ui::UiPoint& pt) const
{
    if (m_pMenuPanel == nullptr || !m_pMenuPanel->IsVisible()) {
        return true;
    }
    // Clicks inside the dropdown must reach the menu items. Clicks on the
    // menu-bar buttons are handled by ToggleMenu(), so let that run instead of
    // hiding the panel here (otherwise clicking the open button could not
    // close it).
    if (m_pMenuPanel->GetPos().ContainsPt(pt) || IsPointInMenuButton(pt)) {
        return true;
    }
    return false;
}

void PolluxOSForm::BuildUi()
{
    ui::VBox* pRoot = new ui::VBox(this);
    /* The wlroots compositor owns the Big Sur wallpaper, every app-window
     * titlebar and every drop shadow. The dui shell paints the macOS-style
     * desktop menu bar, dock and native dropdowns, so its surface is
     * transparent elsewhere; the shell never draws client-side window chrome
     * or shadows. */
    pRoot->SetBkColor(g_pal.transparent);
    pRoot->SetBorderColor(g_pal.transparent);
    pRoot->SetAttribute("border_size", "0");
    pRoot->SetAttribute("padding", "0,0,0,0");

    if (m_menuOverlay) {
        // Keep the menu in its own short surface so fullscreen apps remain
        // visible underneath it when the compositor reveals the top edge.
        BuildMenuBar(pRoot);
    } else if (m_dockOverlay) {
        // The desktop shell is opaque on some Skia/Wayland buffer paths, so
        // keep the dock in its own transparent, always-on-top toplevel.
        BuildDesktopArea(pRoot);
        BuildDock(pRoot);
    } else {
        BuildDesktopArea(pRoot);
    }

    // Native in-window dropdown panel (shared by the desktop right-click
    // quick menu). It is created once, rebuilt per menu and positioned at the
    // click point.
    m_pMenuPanel = new ui::VBox(this);
    m_pMenuPanel->SetFloat(true);
    // Keep the explicitly positioned dropdown where ShowMenuPanel() put it.
    m_pMenuPanel->SetKeepFloatPos(true);
    m_pMenuPanel->SetBkColor(g_pal.menuPanelBg);
    m_pMenuPanel->SetBorderColor(g_pal.menuPanelLine);
    m_pMenuPanel->SetAttribute("border_size", "1");
    m_pMenuPanel->SetAttribute("width", "230");
    m_pMenuPanel->SetAttribute("padding", "6,6,6,6");
    m_pMenuPanel->SetStateColorRound(ui::kControlStateNormal, ui::UiSize(10, 10), false);
    m_pMenuPanel->SetAttribute("border_round", "10,10");
    m_pMenuPanel->SetVisible(false);
    pRoot->AddItem(m_pMenuPanel);

    if (!m_handlersAttached) {
        // One-shot: the callbacks below are appended to the window's event
        // list, so attaching them again on every rebuild would stack
        // duplicates and make a menu close the moment it opened.
        m_handlersAttached = true;
        AttachMenuDismissHandlers();
    }

    AttachBox(pRoot);

    // The dots and chips all live in the tree that has just been replaced.
    ApplyRunningIndicators();
}

void PolluxOSForm::ApplyAppearance()
{
    BuildPalette(m_settings);
}

void PolluxOSForm::PollSettings()
{
    const unsigned long long mtime = pollux::MtimeNs(pollux::ConfigPath());
    if (mtime == m_settingsMtime) {
        return;
    }
    m_settingsMtime = mtime;
    m_settings = pollux::Load();
    ApplyAppearance();
    // Appearance is baked into the controls as they are built, so there is
    // nothing to restyle -- the tree has to be built again.
    m_uiDirty = true;
}

void PolluxOSForm::RebuildUi()
{
    HideMenuPanel();

    // AttachBox() destroys the old tree synchronously, so every pointer into
    // it is dangling the moment BuildUi() runs again. Drop them first; the
    // ones BuildUi() and BuildDock() recreate are reassigned there.
    m_pMenuPanel = nullptr;
    m_pDockBar = nullptr;
    m_dockHitAreaValid = false;
    m_pAppButton = nullptr;
    for (int i = 0; i < kMenuButtonCount; ++i) {
        m_menuButtons[i] = nullptr;
    }
    m_pClockLabel = nullptr;
    m_pDesktopClockLabel = nullptr;
    m_pDesktopDateLabel = nullptr;
    m_dockDots.clear();
    m_dockDotAnchors.clear();
    m_minimizedSlots.clear();
    m_minimizedIds.clear();

    BuildUi();
}

void PolluxOSForm::BuildMenuBar(ui::VBox* pRoot)
{
    ui::HBox* pTopBar = new ui::HBox(this);
    pTopBar->SetAttribute("height", "30");
    pTopBar->SetBkColor(g_pal.barBg);
    pTopBar->SetBorderColor(g_pal.barBorder);
    pTopBar->SetAttribute("bottom_border_size", "1");
    pTopBar->SetAttribute("padding", "12,0,6,0");
    pRoot->AddItem(pTopBar);
    m_pMenuBar = pTopBar;

    // Bold app menu, macOS style (the Apple-menu slot).
    ui::Button* pAppButton = new ui::Button(this);
    pAppButton->SetText("PolluxOS");
    m_pAppButton = pAppButton;
    pAppButton->SetAttribute("font", "system_bold_14");
    pAppButton->SetStateTextColor(ui::kControlStateNormal, g_pal.textDark);
    pAppButton->SetAttribute("text_align", "hcenter,vcenter");
    pAppButton->SetAttribute("height", "24");
    pAppButton->SetAttribute("width", "auto");
    pAppButton->SetAttribute("margin", "0,3,4,3");
    pAppButton->SetAttribute("text_padding", "6,0,6,0");
    pAppButton->SetStateColor(ui::kControlStateNormal, g_pal.transparent);
    pAppButton->SetStateColor(ui::kControlStateHot, g_pal.barHot);
    pAppButton->SetStateColorRound(ui::kControlStateNormal, ui::UiSize(5, 5), false);
    pAppButton->SetStateColorRound(ui::kControlStateHot, ui::UiSize(5, 5), false);
    pAppButton->SetAttribute("cursor_type", "hand");
    // Hover switches menus only after a click has entered menu tracking.
    pAppButton->AttachMouseEnter([this, pAppButton](const ui::EventArgs& /*args*/) {
        if (m_pMenuPanel != nullptr && m_pMenuPanel->IsVisible() &&
            m_openMenuIndex != -1) {
            HideMenuPanel();
            ui::UiRect rc = pAppButton->GetRect();
            ShowMenuPanel(kAppMenu, kMenuBarMenuCounts[0],
                          rc.IsEmpty() ? 12 : rc.left, rc.bottom + 2);
        }
        return true;
    });
    pAppButton->AttachClick([this, pAppButton](const ui::EventArgs& /*args*/) {
        if (m_pMenuPanel != nullptr && m_pMenuPanel->IsVisible() &&
            m_openMenuIndex == -1) {
            HideMenuPanel();
            return true;
        }
        HideMenuPanel();
        ui::UiRect rc = pAppButton->GetRect();
        ShowMenuPanel(kAppMenu, kMenuBarMenuCounts[0],
                      rc.IsEmpty() ? 12 : rc.left, rc.bottom + 2);
        return true;
    });
    pTopBar->AddItem(pAppButton);

    // 文件 / 编辑 / 显示 / 前往 / 窗口 / 帮助.
    for (int mi = 0; mi < kMenuButtonCount; ++mi) {
        ui::Button* pBtn = new ui::Button(this);
        pBtn->SetText(kMenuButtonText[mi]);
        pBtn->SetAttribute("font", "system_14");
        pBtn->SetStateTextColor(ui::kControlStateNormal, g_pal.textDark);
        pBtn->SetAttribute("text_align", "hcenter,vcenter");
        pBtn->SetAttribute("height", "24");
        pBtn->SetAttribute("width", "auto");
        pBtn->SetAttribute("margin", "0,3,2,3");
        pBtn->SetAttribute("text_padding", "6,0,6,0");
        pBtn->SetStateColor(ui::kControlStateNormal, g_pal.transparent);
        pBtn->SetStateColor(ui::kControlStateHot, g_pal.barHot);
        pBtn->SetStateColorRound(ui::kControlStateNormal, ui::UiSize(5, 5), false);
        pBtn->SetStateColorRound(ui::kControlStateHot, ui::UiSize(5, 5), false);
        pBtn->SetAttribute("cursor_type", "hand");
        // Once a click opens a menu, moving across the menu bar switches the
        // active dropdown; idle hovering never opens one.
        pBtn->AttachMouseEnter([this, mi](const ui::EventArgs& /*args*/) {
            if (m_pMenuPanel != nullptr && m_pMenuPanel->IsVisible() &&
                    m_openMenuIndex != mi) {
                ToggleMenu(mi);
            }
            return true;
        });
        pBtn->AttachClick([this, mi](const ui::EventArgs& /*args*/) {
            ToggleMenu(mi);
            return true;
        });
        pTopBar->AddItem(pBtn);
        m_menuButtons[mi] = pBtn;
    }

    // Right side: spacer pushes the window controls + clock to the right edge.
    ui::Control* pTopSpacer = new ui::Control(this);
    pTopSpacer->SetAttribute("width", "stretch");
    pTopSpacer->SetAttribute("mouse_enabled", "false");
    pTopBar->AddItem(pTopSpacer);

    // Status glyphs, as macOS has them: small monochrome icons rather than
    // words. dui cannot tint an SVG, so each ships in a light and a dark
    // version and the palette says which to load.
    auto AddStatusGlyph = [this, pTopBar](const char* glyph,
                                          int buttonWidth, int imageWidth) {
        ui::Button* pButton = new ui::Button(this);
        pButton->SetAttribute("width", Num(buttonWidth));
        pButton->SetAttribute("height", "24");
        pButton->SetAttribute("cursor_type", "hand");
        pButton->SetBkImage(U8String("file='polluxdesk/icons/status_") + glyph +
                            "_" + g_pal.glyphVariant +
                            ".svg' width='" + Num(imageWidth) +
                            "' height='19' halign='center' valign='center'");
        pButton->SetStateColor(ui::kControlStateNormal, g_pal.transparent);
        pButton->SetStateColor(ui::kControlStateHot, g_pal.barHot);
        pButton->SetStateColorRound(ui::kControlStateNormal, ui::UiSize(5, 5), false);
        pButton->SetStateColorRound(ui::kControlStateHot, ui::UiSize(5, 5), false);
        pButton->SetAttribute("border_round", "5,5");
        pTopBar->AddItem(pButton);
        return pButton;
    };

    // The status items are buttons, not decoration: each one opens a panel of
    // real controls, as on macOS.
    ui::Button* pWifi = AddStatusGlyph("wifi", 30, 22);
    ui::Button* pVolume = AddStatusGlyph("volume", 38, 30);
    ui::Button* pBattery = AddStatusGlyph("battery", 38, 30);
    // Each handler opens its panel directly rather than through a shared
    // helper: the click callbacks outlive this function, so anything they
    // captured by reference would dangle the moment it returned.
    pWifi->AttachClick([this, pWifi](const ui::EventArgs&) {
        const ui::UiRect rc = pWifi->GetRect();
        HideMenuPanel();
        ShowWifiPanel(rc.right - kPopoverWidth, rc.bottom + 4);
        return true;
    });
    pVolume->AttachClick([this, pVolume](const ui::EventArgs&) {
        const ui::UiRect rc = pVolume->GetRect();
        HideMenuPanel();
        ShowVolumePanel(rc.right - kPopoverWidth, rc.bottom + 4);
        return true;
    });
    pBattery->AttachClick([this, pBattery](const ui::EventArgs&) {
        const ui::UiRect rc = pBattery->GetRect();
        HideMenuPanel();
        ShowBatteryPanel(rc.right - kPopoverWidth, rc.bottom + 4);
        return true;
    });

    // The reading sits against its own glyph, the way macOS shows it when the
    // percentage is switched on.
    m_pBatteryLabel = new ui::Label(this);
    m_pBatteryLabel->SetAttribute("font", "system_12");
    m_pBatteryLabel->SetStateTextColor(ui::kControlStateNormal, g_pal.textDark);
    m_pBatteryLabel->SetAttribute("text_align", "hcenter,vcenter");
    m_pBatteryLabel->SetAttribute("width", "42");
    m_pBatteryLabel->SetAttribute("height", "24");
    m_pBatteryLabel->SetText("--%");
    m_pBatteryLabel->SetMouseEnabled(false);
    pTopBar->AddItem(m_pBatteryLabel);

    // Not a Spotlight field: the desktop shell never takes keyboard focus --
    // the compositor skips it deliberately so that clicking the wallpaper
    // does not steal the keyboard from the window in front -- so there is
    // nothing here that could receive typing. It opens the app grid instead.
    ui::Button* pSearch = AddStatusGlyph("search", 38, 30);
    pSearch->SetToolTipText("打开启动台");
    pSearch->AttachClick([this](const ui::EventArgs&) {
        HideMenuPanel();
        ShowLaunchPad();
        return true;
    });

    ui::Button* controlCenter = new ui::Button(this);
    controlCenter->SetAttribute("height", "24");
    controlCenter->SetAttribute("width", "40");
    controlCenter->SetAttribute("margin", "4,3,4,3");
    controlCenter->SetBkImage(U8String("file='polluxdesk/icons/status_controlcenter_") +
                              g_pal.glyphVariant +
                              ".svg' width='27' height='17' halign='center' valign='center'");
    controlCenter->SetStateColor(ui::kControlStateNormal, g_pal.transparent);
    controlCenter->SetStateColor(ui::kControlStateHot, g_pal.barHot);
    controlCenter->SetStateColorRound(ui::kControlStateNormal, ui::UiSize(8, 8), false);
    controlCenter->SetStateColorRound(ui::kControlStateHot, ui::UiSize(8, 8), false);
    controlCenter->SetStateColorRound(ui::kControlStatePushed, ui::UiSize(8, 8), false);
    controlCenter->SetAttribute("border_round", "8,8");
    controlCenter->SetAttribute("cursor_type", "hand");
    controlCenter->SetToolTipText("控制中心");
    controlCenter->AttachClick([this, controlCenter](const ui::EventArgs&) {
        const ui::UiRect rc = controlCenter->GetRect();
        HideMenuPanel();
        ShowControlCentre(rc.right - kPopoverWidth, rc.bottom + 4);
        return true;
    });
    pTopBar->AddItem(controlCenter);

    // The clock is last, at the very right edge, as on macOS.
    m_pClockLabel = new ui::Label(this);
    m_pClockLabel->SetAttribute("font", "system_14");
    m_pClockLabel->SetStateTextColor(ui::kControlStateNormal, g_pal.textDark);
    m_pClockLabel->SetAttribute("text_align", "hcenter,vcenter");
    m_pClockLabel->SetAttribute("width", "200");
    m_pClockLabel->SetAttribute("height", "24");
    m_pClockLabel->SetAttribute("margin", "0,3,0,3");
    m_pClockLabel->SetText("--月--日 周- --:--");
    m_pClockLabel->SetMouseEnabled(false);
    pTopBar->AddItem(m_pClockLabel);
}

void PolluxOSForm::BuildDesktopArea(ui::VBox* pRoot)
{
    // The desktop area is just the wallpaper: no top bar, no clock/date in the
    // middle. It only provides the desktop right-click quick menu and dismisses
    // any open dropdown on a plain left click.
    ui::VBox* pDesktop = new ui::VBox(this);
    pDesktop->SetAttribute("height", "stretch");
    pDesktop->SetAttribute("mouse_enabled", "true");
    pRoot->AddItem(pDesktop);

    // A plain left click on the wallpaper dismisses any open dropdown,
    // exactly like clicking the macOS desktop. AttachButtonDown is used
    // because generic Box controls do not synthesize kEventClick.
    pDesktop->AttachButtonDown([this](const ui::EventArgs& /*args*/) {
        HideMenuPanel();
        HideAppPanel();
        return true;
    });
    pDesktop->AttachRClick([this](const ui::EventArgs& args) {
        ShowQuickMenu(args.ptMouse.x, args.ptMouse.y);
        return true;
    });
}

// The compositor identifies a window's program by the client's process id:
// dui hard-codes its app_id to one value for every window it opens, and the
// title follows the document rather than the program.
static std::string ExeNameForPid(long pid)
{
    if (pid <= 0) {
        return std::string();
    }
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, static_cast<int>(pid) };
    struct kinfo_proc info;
    size_t size = sizeof(info);
    if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0 || size == 0) {
        return std::string();
    }
    return std::string(info.ki_comm);
}

static std::vector<std::string> SplitFields(const std::string& line, char sep)
{
    std::vector<std::string> fields;
    size_t start = 0;
    while (true) {
        size_t end = line.find(sep, start);
        if (end == std::string::npos) {
            fields.push_back(line.substr(start));
            return fields;
        }
        fields.push_back(line.substr(start, end - start));
        start = end + 1;
    }
}

void PolluxOSForm::PollWindowState()
{
    const std::string path = pollux::StatePath();
    const unsigned long long mtime = pollux::MtimeNs(path);
    if (mtime == m_stateMtime) {
        return;   // nothing has been written since the last look
    }
    m_stateMtime = mtime;

    const std::vector<std::string> lines = pollux::ReadLines(path);
    m_windows.clear();

    // Trust the list only while the compositor that wrote it is alive. A
    // compositor restart leaves the file naming a process that no longer
    // exists, and its windows went with it.
    const std::string pidText = pollux::ValueOf(lines, "desktop_pid");
    const long compositorPid = pidText.empty() ? 0 : std::atol(pidText.c_str());
    if (compositorPid <= 0 || ::kill(static_cast<pid_t>(compositorPid), 0) != 0) {
        ApplyRunningIndicators();
        return;
    }

    m_thumbPending = false;
    for (const std::string& line : lines) {
        if (line.compare(0, 6, "thumb=") == 0) {
            // id|x|y|width|height -- a minimized window the compositor is
            // still showing while we take its picture.
            const std::vector<std::string> f = SplitFields(line.substr(6), '|');
            if (f.size() >= 5) {
                m_thumbId = std::strtoul(f[0].c_str(), nullptr, 10);
                m_thumbX = std::atoi(f[1].c_str());
                m_thumbY = std::atoi(f[2].c_str());
                m_thumbW = std::atoi(f[3].c_str());
                m_thumbH = std::atoi(f[4].c_str());
                m_thumbPending = m_thumbW > 0 && m_thumbH > 0;
            }
            continue;
        }
        if (line.compare(0, 4, "win=") != 0) {
            continue;
        }
        // id|app_id|title|minimized|focused|width|height|pid
        const std::vector<std::string> f = SplitFields(line.substr(4), '|');
        if (f.size() < 8) {
            continue;   // truncated or hand-edited; skip rather than guess
        }
        WindowInfo info;
        info.id        = std::strtoul(f[0].c_str(), nullptr, 10);
        info.appId     = f[1];
        info.title     = f[2];
        info.minimized = f[3] == "1";
        info.focused   = f[4] == "1";
        info.width     = std::atoi(f[5].c_str());
        info.height    = std::atoi(f[6].c_str());
        info.pid       = std::atol(f[7].c_str());
        info.exe       = ExeNameForPid(info.pid);
        m_windows.push_back(info);
    }
    ApplyRunningIndicators();
    UpdateMinimizedShelf();
    UpdateRunningApps();
}

void PolluxOSForm::UpdateMinimizedShelf()
{
    std::vector<unsigned long> ids;
    for (const WindowInfo& info : m_windows) {
        if (info.minimized &&
                ids.size() < static_cast<size_t>(kMinimizedSlots)) {
            ids.push_back(info.id);
        }
    }
    if (ids == m_shelfIds) {
        return;   // nothing moved
    }
    m_shelfIds = ids;

    // The shelf is built by BuildDock(), so a change to it means the dock has
    // to be built again -- dui will not recompute a laid-out rect just
    // because a control was resized afterwards. Rebuilding here would be a
    // use-after-free (AttachBox deletes the tree synchronously and this runs
    // from a state poll), so it is deferred to the next timer tick.
    m_uiDirty = true;
}

void PolluxOSForm::UpdateRunningApps()
{
    std::vector<unsigned long> ids;
    for (const WindowInfo& info : m_windows) {
        if (info.exe.empty() || HasPinnedTile(info.exe)) {
            continue;   // it already has a tile, and a dot on it
        }
        if (ids.size() >= static_cast<size_t>(kRunningTileSlots)) {
            break;
        }
        ids.push_back(info.id);
    }
    if (ids == m_runningIds) {
        return;   // same programs as last time
    }
    m_runningIds = ids;
    m_uiDirty = true;   // BuildDock() is what draws these
}

// A dock tile's right-click menu.
//
// The commands are written into fixed buffers because MenuItem holds a
// `const char*` and the panel's click handler keeps that pointer: a
// std::string would move its storage on the next reassignment and leave the
// handler reading whatever landed in the old one.
void PolluxOSForm::ShowDockMenu(const std::string& title, const std::string& pids,
                                int x, int y)
{
    if (pids.empty()) {
        return;
    }
    static char s_heading[512];
    static char s_quitCmd[512];
    static char s_forceCmd[512];
    std::snprintf(s_heading, sizeof(s_heading), "%s", title.c_str());
    std::snprintf(s_quitCmd, sizeof(s_quitCmd), "kill %s", pids.c_str());
    std::snprintf(s_forceCmd, sizeof(s_forceCmd), "kill -9 %s", pids.c_str());

    static MenuItem items[4];
    items[0] = { s_heading,  nullptr,    false, false };   // what is being quit
    items[1] = { "",         nullptr,    true,  false };
    items[2] = { "退出",      s_quitCmd,  false, true };
    items[3] = { "强制退出",  s_forceCmd, false, true };

    HideMenuPanel();
    ShowMenuPanel(items, 4, x, y);
}

void PolluxOSForm::GrabPendingThumbnail()
{
    if (!m_thumbPending) {
        return;
    }
    // One attempt per request: if grim fails the window simply gets a plain
    // chip instead of a picture, which is better than retrying at it.
    m_thumbPending = false;

    const unsigned long id = m_thumbId;
    pollux::MkdirP(pollux::ThumbDir());
    const std::string path = pollux::ThumbPath(id);

    // grim reads the composited output, so the rectangle the compositor
    // published is exactly what it wants.
    const std::string cmd = "grim -g \"" + std::to_string(m_thumbX) + "," +
        std::to_string(m_thumbY) + " " + std::to_string(m_thumbW) + "x" +
        std::to_string(m_thumbH) + "\" '" + path + "' 2>/dev/null";
    // Deliberately synchronous: the compositor is holding the window on
    // screen until we answer, so the grab has to be finished first. It costs
    // a fraction of a second, once, on a click that was going to hide the
    // window anyway.
    pollux::Run(cmd.c_str());

    SetText(ui::StringUtil::Printf("PolluxOS Desktop (thumb-ready:%lu)", id));
    m_titleMarkerPending = true;
    m_uiDirty = true;   // the shelf can show the picture on the next build
}

// A tile's `exe` field is a "|"-separated list of program names, so one tile
// can stand for any of several programs -- the browser tile covers whichever
// of firefox/chromium got installed.
static bool ExeListHas(const char* exeList, const std::string& exe)
{
    if (exeList == nullptr || exeList[0] == '\0' || exe.empty()) {
        return false;
    }
    const std::string list(exeList);
    size_t start = 0;
    while (start <= list.size()) {
        size_t end = list.find('|', start);
        if (end == std::string::npos) {
            end = list.size();
        }
        if (list.compare(start, end - start, exe) == 0) {
            return true;
        }
        start = end + 1;
    }
    return false;
}

bool PolluxOSForm::IsAppRunning(const char* exeList) const
{
    for (const WindowInfo& info : m_windows) {
        if (ExeListHas(exeList, info.exe)) {
            return true;
        }
    }
    return false;
}

bool PolluxOSForm::HasPinnedTile(const std::string& exe)
{
    const int count = static_cast<int>(sizeof(kDockApps) / sizeof(kDockApps[0]));
    for (int i = 0; i < count; ++i) {
        if (ExeListHas(kDockApps[i].exe, exe)) {
            return true;
        }
    }
    return false;
}

void PolluxOSForm::ApplyRunningIndicators()
{
    const int count = static_cast<int>(sizeof(kDockApps) / sizeof(kDockApps[0]));
    for (size_t i = 0; i < m_dockDots.size() && static_cast<int>(i) < count; ++i) {
        if (m_dockDots[i] == nullptr) {
            continue;
        }
        m_dockDots[i]->SetBkColor(IsAppRunning(kDockApps[i].exe) ? g_pal.dockDot
                                                                 : g_pal.transparent);
        m_dockDots[i]->Invalidate();
    }
}

// dui takes a corner radius through two independent APIs that have to agree:
// the state colours carry their own rounding, and border_round draws the
// border. Setting one and not the other silently paints two different
// corners, so every rounded control here goes through this helper.
//
// `interactive` covers the controls that also define hot/pushed states --
// those states round separately and would otherwise square off on hover.
static void SetRadius(ui::Control* pControl, int radius, bool interactive)
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

// The trash can, drawn out of dui primitives. The resource root is pinned to
// dui's own resources, so an extra icon file cannot be added; the tile has to
// be assembled from boxes the way the folder and document glyphs in the file
// browser are.
//
// Its gradient is the one the Settings icon uses, so the two "system" tiles
// read as a pair, with the can itself drawn light against it.
static ui::ButtonVBox* MakeTrashTile(ui::Window* pWindow, int size)
{
    // Every part is a fraction of the tile so the can scales with it.
    const auto scale = [size](int num, int den) { return size * num / den; };

    // macOS draws the trash as a translucent can standing on the dock, with
    // no tile behind it -- so this one follows the appearance instead of
    // carrying the Settings tile's grey.
    const U8String canBody   = g_pal.dockCanBody;
    const U8String canEdge   = g_pal.dockCanEdge;

    ui::ButtonVBox* pTile = new ui::ButtonVBox(pWindow);
    pTile->SetAttribute("height", Num(size));
    pTile->SetAttribute("width", Num(size));
    pTile->SetBkColor(g_pal.transparent);
    pTile->SetBorderColor(ui::kControlStateNormal, g_pal.transparent);
    pTile->SetBorderColor(ui::kControlStateHot, g_pal.accent);
    pTile->SetAttribute("cursor_type", "hand");
    pTile->SetToolTipText("废纸篓");
    SetRadius(pTile, scale(14, 56), true);

    // dui splits centring in a VBox between the layout and the child:
    // child_align="vcenter" offsets the whole run of children vertically, but
    // there is no horizontal equivalent, so each part carries its own
    // halign="center". With only the layout attribute the can sits against
    // the tile's left edge.
    pTile->SetAttribute("child_align", "vcenter");

    // Lid handle, lid, then the body: stacked vertically so the can grows
    // from the top down as the icon size changes.
    const int handleH = std::max(2, scale(3, 56));
    const int lidH    = std::max(3, scale(4, 56));
    const int bodyW   = scale(22, 56);
    const int bodyH   = scale(26, 56);

    ui::Control* pHandle = new ui::Control(pWindow);
    pHandle->SetAttribute("halign", "center");
    pHandle->SetAttribute("width", Num(scale(10, 56)));
    pHandle->SetAttribute("height", Num(handleH));
    pHandle->SetBkColor(canBody);
    pHandle->SetMouseEnabled(false);
    SetRadius(pHandle, std::max(1, handleH / 2), false);
    pTile->AddItem(pHandle);

    ui::Control* pLid = new ui::Control(pWindow);
    pLid->SetAttribute("halign", "center");
    pLid->SetAttribute("width", Num(scale(26, 56)));
    pLid->SetAttribute("height", Num(lidH));
    pLid->SetBkColor(canBody);
    pLid->SetBorderColor(canEdge);
    pLid->SetAttribute("border_size", "1");
    pLid->SetMouseEnabled(false);
    SetRadius(pLid, std::max(1, lidH / 2), false);
    pTile->AddItem(pLid);

    ui::Control* pGap = new ui::Control(pWindow);
    pGap->SetAttribute("halign", "center");
    pGap->SetAttribute("height", Num(std::max(1, scale(2, 56))));
    pGap->SetMouseEnabled(false);
    pTile->AddItem(pGap);

    // HBox so the three ribs stand upright.
    ui::HBox* pBody = new ui::HBox(pWindow);
    pBody->SetAttribute("halign", "center");
    pBody->SetAttribute("width", Num(bodyW));
    pBody->SetAttribute("height", Num(bodyH));
    pBody->SetBkColor(canBody);
    pBody->SetBkColor2(canEdge);
    pBody->SetBkColor2Direction("1");
    pBody->SetBorderColor(canEdge);
    pBody->SetAttribute("border_size", "1");
    pBody->SetAttribute("child_align", "hcenter,vcenter");
    pBody->SetMouseEnabled(false);
    SetRadius(pBody, std::max(2, scale(4, 56)), false);

    for (int i = 0; i < 3; ++i) {
        ui::Control* pRib = new ui::Control(pWindow);
        pRib->SetAttribute("width", "1");
        pRib->SetAttribute("height", Num(std::max(4, bodyH - scale(8, 56))));
        pRib->SetAttribute("margin", MarginH(std::max(1, scale(2, 56))));
        pRib->SetBkColor(canEdge);
        pRib->SetMouseEnabled(false);
        pBody->AddItem(pRib);
    }
    pTile->AddItem(pBody);

    return pTile;
}

// ---------------------------------------------------------------------------
// Status popovers
//
// Everything on the right of the menu bar reads real hardware: the same mixer,
// ifconfig and sysctl the settings app uses. Nothing here is a mock toggle --
// where a control would need root (bringing a wireless interface up, for
// instance) it is not offered rather than being faked.
// ---------------------------------------------------------------------------
// Left-aligned line of text inside a popover.
static ui::Label* AddPopoverText(ui::Window* pWindow, ui::VBox* pPanel,
                                 const U8String& text, const U8String& colour,
                                 const U8String& font)
{
    ui::Label* pLabel = new ui::Label(pWindow);
    pLabel->SetText(text);
    pLabel->SetAttribute("font", font);
    pLabel->SetStateTextColor(ui::kControlStateNormal, colour);
    pLabel->SetAttribute("text_align", "left,vcenter");
    pLabel->SetAttribute("width", "stretch");
    pLabel->SetAttribute("height", Num(kPopoverRowH));
    pLabel->SetMouseEnabled(false);
    pPanel->AddItem(pLabel);
    return pLabel;
}

// A section divider with a dim heading, which is how macOS groups a popover.
static void AddPopoverSection(ui::Window* pWindow, ui::VBox* pPanel,
                              const U8String& text)
{
    ui::Control* pLine = new ui::Control(pWindow);
    pLine->SetAttribute("height", "1");
    pLine->SetAttribute("width", "stretch");
    pLine->SetAttribute("margin", "0,8,0,4");
    pLine->SetBkColor(g_pal.menuPanelLine);
    pLine->SetMouseEnabled(false);
    pPanel->AddItem(pLine);

    ui::Label* pTitle = AddPopoverText(pWindow, pPanel, text, g_pal.textHint,
                                       "system_12");
    pTitle->SetAttribute("height", "20");
}

// A round-ish chip used for the accent and wallpaper swatches.
static ui::Button* MakeSwatch(ui::Window* pWindow, const U8String& colour,
                              bool selected)
{
    ui::Button* pSwatch = new ui::Button(pWindow);
    pSwatch->SetAttribute("width", "34");
    pSwatch->SetAttribute("height", "24");
    pSwatch->SetAttribute("margin", "0,0,8,0");
    pSwatch->SetAttribute("cursor_type", "hand");
    pSwatch->SetStateColor(ui::kControlStateNormal, colour);
    pSwatch->SetStateColor(ui::kControlStateHot, colour);
    pSwatch->SetBorderColor(ui::kControlStateNormal,
                            selected ? g_pal.accent : g_pal.chipBorder);
    pSwatch->SetBorderColor(ui::kControlStateHot, g_pal.accent);
    pSwatch->SetAttribute("border_size", "2");
    SetRadius(pSwatch, 6, true);
    return pSwatch;
}

// mixer reports vol.volume as "vol.volume=1.00:1.00"; the first number is the
// level. Returns 0-100, or -1 when the machine has no mixer at all.
static int ReadVolumePercent()
{
    const std::string out = pollux::Run("mixer vol.volume 2>/dev/null");
    const size_t eq = out.find('=');
    if (eq == std::string::npos) {
        return -1;
    }
    double value = std::atof(out.c_str() + eq + 1);
    int percent = static_cast<int>(value * 100.0 + 0.5);
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    return percent;
}

static bool ReadVolumeMuted()
{
    const std::string out = pollux::Run("mixer vol.mute 2>/dev/null");
    return out.find("=on") != std::string::npos;
}

// The SSID the wireless interface is associated with, or empty.
static std::string CurrentSsid()
{
    const std::string out = pollux::Run("ifconfig wlan0 2>/dev/null");
    const size_t pos = out.find("ssid ");
    if (pos == std::string::npos) {
        return std::string();
    }
    const size_t start = pos + 5;
    const size_t end = out.find(' ', start);
    return out.substr(start, end == std::string::npos ? std::string::npos
                                                      : end - start);
}

// Double-quote a word for `sh -c`, so an SSID can be passed to the Wi-Fi
// window as an argument whatever characters it contains.
static std::string ShellQuote(const std::string& text)
{
    std::string out = "\"";
    for (char ch : text) {
        if (ch == '"' || ch == '\\' || ch == '$' || ch == '`') {
            out += '\\';
        }
        out += ch;
    }
    out += '"';
    return out;
}

static bool AcOnline()
{
    return pollux::Run("sysctl -n hw.acpi.acline 2>/dev/null").find('1') !=
        std::string::npos;
}

void PolluxOSForm::ShowPopover(int x, int y, int width,
                               const std::function<int(ui::VBox* panel)>& build)
{
    if (m_pMenuPanel == nullptr) {
        return;
    }
    m_pMenuPanel->RemoveAllItems();
    const int height = build(m_pMenuPanel);

    ui::UiRect client;
    GetClientRect(client);
    int px = x;
    int py = y;
    if (px + width > client.right) {
        px = client.right - width - 8;
    }
    if (py + height > client.bottom) {
        py = client.bottom - height - 8;
    }
    if (px < 4) {
        px = 4;
    }
    if (py < 30) {
        py = 30;
    }

    m_pMenuPanel->SetAttribute("width", Num(width));
    m_pMenuPanel->SetAttribute("height", Num(height));
    m_pMenuPanel->SetPos(ui::UiRect(px, py, px + width, py + height));
    m_pMenuPanel->SetPaintOrder(100);
    m_pMenuPanel->SetVisible(true);
    // Raise the shell above app windows for as long as the panel is open.
    SetCompositorMenuOpen(true, py + height + 8);
    Invalidate(m_pMenuPanel->GetPos());
}

void PolluxOSForm::ShowVolumePanel(int x, int y)
{
    ShowPopover(x, y, kPopoverWidth, [this](ui::VBox* pPanel) -> int {
        int h = kPopoverPad * 2;
        AddPopoverText(this, pPanel, "声音", g_pal.textDark, "system_15");
        h += kPopoverRowH;

        int percent = ReadVolumePercent();
        if (percent < 0) {
            AddPopoverText(this, pPanel, "这台机器没有可用的混音器",
                           g_pal.textHint, "system_13");
            return h + kPopoverRowH;
        }

        // A slider and its reading, side by side.
        ui::HBox* pRow = new ui::HBox(this);
        pRow->SetAttribute("width", "stretch");
        pRow->SetAttribute("height", "26");

        ui::Slider* pSlider = new ui::Slider(this);
        pSlider->SetClass("slider_horizontal_green");
        pSlider->SetAttribute("width", "stretch");
        pSlider->SetAttribute("height", "26");
        pSlider->SetAttribute("thumb_size", "16,16");
        pSlider->SetAttribute("progress_color", g_pal.accent);
        pSlider->SetMinValue(0);
        pSlider->SetMaxValue(100);
        pSlider->SetValue(percent);
        pSlider->SetAttribute("cursor_type", "hand");
        ui::Label* pValue = new ui::Label(this);
        pValue->SetAttribute("font", "system_12");
        pValue->SetStateTextColor(ui::kControlStateNormal, g_pal.textBody);
        pValue->SetAttribute("text_align", "right,vcenter");
        pValue->SetAttribute("width", "44");
        pValue->SetText(Num(percent) + "%");

        pSlider->AttachValueChanged([this, pValue](const ui::EventArgs& args) {
            int value = static_cast<int>(args.wParam);
            if (value < 0) value = 0;
            if (value > 100) value = 100;
            pValue->SetText(Num(value) + "%");
            pollux::Run(("mixer vol=" + std::to_string(value) + "%").c_str());
            return true;
        });

        pRow->AddItem(pSlider);
        pRow->AddItem(pValue);
        pPanel->AddItem(pRow);
        h += 26;

        ui::CheckBox* pMute = new ui::CheckBox(this);
        pMute->SetText("静音");
        pMute->SetAttribute("font", "system_13");
        pMute->SetStateTextColor(ui::kControlStateNormal, g_pal.textBody);
        pMute->SetAttribute("height", Num(kPopoverRowH));
        pMute->SetAttribute("cursor_type", "hand");
        pMute->Selected(ReadVolumeMuted());
        pMute->AttachSelect([this](const ui::EventArgs&) {
            const bool muted = !ReadVolumeMuted();
            pollux::Run(muted ? "mixer vol.mute=on" : "mixer vol.mute=off");
            return true;
        });
        pPanel->AddItem(pMute);
        return h + kPopoverRowH;
    });
}

void PolluxOSForm::ShowWifiPanel(int x, int y)
{
    ShowPopover(x, y, kPopoverWidth, [this](ui::VBox* pPanel) -> int {
        int h = kPopoverPad * 2;
        AddPopoverText(this, pPanel, "Wi-Fi", g_pal.textDark, "system_15");
        h += kPopoverRowH;

        // The same data layer the Wi-Fi window uses, so the two always agree
        // on the interface, the networks around it and which of them are
        // saved.  This is what makes the panel able to switch networks: the
        // readings and the join go through one implementation.
        wifi::Wifi wifi;
        wifi::Status status;
        wifi.ReadStatus(status);
        AddPopoverText(this, pPanel,
                       status.ssid.empty() ? U8String("未连接") : U8String(status.ssid.c_str()),
                       status.ssid.empty() ? g_pal.textHint : g_pal.textBody,
                       "system_13");
        h += kPopoverRowH;

        std::vector<wifi::Network> networks;
        std::string scanError;
        const bool scanned = wifi.Scan(networks, scanError);
        AddPopoverSection(this, pPanel, "网络");
        h += 1 + 8 + 4 + 20;   // the divider and its margins, then the heading

        if (!scanned) {
            AddPopoverText(this, pPanel, U8String(scanError.c_str()), g_pal.textHint,
                           "system_13");
            h += kPopoverRowH;
        } else if (networks.empty()) {
            AddPopoverText(this, pPanel, "没有扫描到网络", g_pal.textHint,
                           "system_13");
            h += kPopoverRowH;
        }

        const size_t kMaxRows = 8;
        for (size_t i = 0; i < networks.size() && i < kMaxRows; ++i) {
            const wifi::Network& network = networks[i];
            const bool current = !status.ssid.empty() && network.ssid == status.ssid;

            // Without the control socket nothing can be changed from here
            // either, so the list stays plain text with the reason below.
            if (!status.haveControl) {
                AddPopoverText(this, pPanel, U8String(network.ssid.c_str()),
                               current ? g_pal.textBody : g_pal.textHint,
                               "system_13");
                h += kPopoverRowH;
                continue;
            }

            // A click is all a switch takes for anything that needs no
            // password: open networks, and saved ones whose key is already
            // stored.  A new secured network is handed to the Wi-Fi window,
            // because a password needs the keyboard and this panel is part of
            // the shell, which the compositor never gives one.
            ui::ButtonHBox* pRow = new ui::ButtonHBox(this);
            pRow->SetAttribute("height", "30");
            pRow->SetAttribute("width", "stretch");
            pRow->SetAttribute("padding", "8,0,8,0");
            pRow->SetAttribute("cursor_type", "hand");
            pRow->SetStateColor(ui::kControlStateNormal,
                                current ? g_pal.barHot : g_pal.transparent);
            pRow->SetStateColor(ui::kControlStateHot, g_pal.menuItemHot);
            SetRadius(pRow, 7, true);
            pPanel->AddItem(pRow);
            h += 30;

            ui::Label* pName = new ui::Label(this);
            pName->SetText(U8String(network.ssid.c_str()));
            pName->SetAttribute("font", "system_13");
            pName->SetStateTextColor(ui::kControlStateNormal, g_pal.textBody);
            pName->SetAttribute("text_align", "left,vcenter");
            pName->SetAttribute("width", "stretch");
            pName->SetMouseEnabled(false);
            pRow->AddItem(pName);

            ui::Label* pState = new ui::Label(this);
            pState->SetText(current ? "已连接"
                                    : (network.saved
                                           ? "已保存"
                                           : (network.secured ? "需密码" : "")));
            pState->SetAttribute("font", "system_12");
            pState->SetStateTextColor(ui::kControlStateNormal,
                                      current ? g_pal.accent : g_pal.textHint);
            pState->SetAttribute("text_align", "right,vcenter");
            pState->SetAttribute("width", "60");
            pState->SetMouseEnabled(false);
            pRow->AddItem(pState);

            const std::string ssid = network.ssid;
            const bool saved = network.saved;
            const bool secured = network.secured;
            pRow->AttachClick([this, ssid, saved, secured, current](const ui::EventArgs&) {
                if (current) {
                    HideMenuPanel();
                    return true;
                }
                if (saved || !secured) {
                    // No keyboard needed: the stored key (or none at all)
                    // joins the network, and the menu bar shows the new one
                    // on its next status tick.
                    HideMenuPanel();
                    wifi::Wifi wifi;
                    std::string error;
                    wifi.Connect(ssid, std::string(), secured, error);
                    return true;
                }
                // The password has to be typed where the keyboard goes, and
                // the window opens with this network already picked.
                const std::string command = std::string("\"") + POLLUX_BIN +
                                            "/polluxdesk_wifi\" " + ShellQuote(ssid);
                LaunchApp(command.c_str());
                return true;
            });
        }

        AddPopoverSection(this, pPanel, "");
        h += 1 + 8 + 4 + 20;
        // The keyboard, not privilege, is what this panel lacks: the shell is
        // the bottom-most surface and the compositor never focuses it, so a
        // password can only be typed into a window of its own.
        AddPopoverText(this, pPanel,
                       status.haveControl
                           ? "需密码的网络会打开 Wi-Fi 窗口输入"
                           : "只读：无法在此更改网络，原因见 Wi-Fi 窗口",
                       g_pal.textHint, "system_12");
        h += kPopoverRowH;

        ui::Button* pOpen = new ui::Button(this);
        pOpen->SetText("打开 Wi-Fi 设置…");
        pOpen->SetAttribute("font", "system_13");
        pOpen->SetAttribute("width", "stretch");
        pOpen->SetAttribute("height", "30");
        pOpen->SetAttribute("margin", "0,10,0,0");
        pOpen->SetAttribute("cursor_type", "hand");
        pOpen->SetStateColor(ui::kControlStateNormal, g_pal.accent);
        pOpen->SetStateColor(ui::kControlStateHot, g_pal.accent);
        pOpen->SetStateTextColor(ui::kControlStateNormal, "#FFFFFFFF");
        SetRadius(pOpen, 7, true);
        pOpen->AttachClick([this](const ui::EventArgs&) {
            HideMenuPanel();
            LaunchApp("\"" POLLUX_BIN "/polluxdesk_wifi\"");
            return true;
        });
        pPanel->AddItem(pOpen);
        h += 40;
        return h;
    });
}

void PolluxOSForm::ShowBatteryPanel(int x, int y)
{
    ShowPopover(x, y, kPopoverWidth, [this](ui::VBox* pPanel) -> int {
        int h = kPopoverPad * 2;
        AddPopoverText(this, pPanel, "电池", g_pal.textDark, "system_15");
        h += kPopoverRowH;

        std::string life = pollux::Trim(pollux::Run("sysctl -n hw.acpi.battery.life 2>/dev/null"));
        std::string time = pollux::Trim(pollux::Run("sysctl -n hw.acpi.battery.time 2>/dev/null"));
        const bool online = AcOnline();

        if (life.empty()) {
            AddPopoverText(this, pPanel, "这台机器没有电池", g_pal.textHint,
                           "system_13");
            return h + kPopoverRowH;
        }

        AddPopoverText(this, pPanel, U8String(life.c_str()) + "%",
                       g_pal.textDark, "system_20");
        h += kPopoverRowH + 6;

        const int minutes = std::atoi(time.c_str());
        if (minutes > 0) {
            AddPopoverText(this, pPanel,
                           online ? U8String("充满还需 ") + Num(minutes) + " 分钟"
                                  : U8String("剩余 ") + Num(minutes) + " 分钟",
                           g_pal.textBody, "system_13");
            h += kPopoverRowH;
        }
        AddPopoverText(this, pPanel,
                       online ? "已接通电源" : "使用电池供电",
                       g_pal.textHint, "system_13");
        return h + kPopoverRowH;
    });
}

void PolluxOSForm::ShowControlCentre(int x, int y)
{
    ShowPopover(x, y, kPopoverWidth, [this](ui::VBox* pPanel) -> int {
        int h = kPopoverPad * 2;
        AddPopoverText(this, pPanel, "控制中心", g_pal.textDark, "system_15");
        h += kPopoverRowH;

        // --- appearance ---------------------------------------------------
        AddPopoverSection(this, pPanel, "外观");
        h += 1 + 8 + 4 + 20;
        {
            ui::HBox* pRow = new ui::HBox(this);
            pRow->SetAttribute("width", "stretch");
            pRow->SetAttribute("height", "28");
            const char* kLabels[] = { "浅色", "深色", "自动" };
            const char* kValues[] = { "light", "dark", "auto" };
            for (int i = 0; i < 3; ++i) {
                ui::Button* pChoice = new ui::Button(this);
                pChoice->SetText(U8String(kLabels[i]));
                pChoice->SetAttribute("font", "system_13");
                pChoice->SetAttribute("width", "stretch");
                pChoice->SetAttribute("height", "28");
                pChoice->SetAttribute("margin", "0,0,6,0");
                pChoice->SetAttribute("cursor_type", "hand");
                const bool active = m_settings.appearance == kValues[i];
                pChoice->SetStateColor(ui::kControlStateNormal,
                                       active ? g_pal.accent : g_pal.chipBg);
                pChoice->SetStateColor(ui::kControlStateHot, g_pal.chipHot);
                pChoice->SetStateTextColor(ui::kControlStateNormal,
                                           active ? "#FFFFFFFF" : g_pal.textBody);
                SetRadius(pChoice, 6, true);
                const std::string value = kValues[i];
                pChoice->AttachClick([this, value](const ui::EventArgs&) {
                    pollux::SaveKey(pollux::kKeyAppearance, value);
                    HideMenuPanel();
                    return true;
                });
                pRow->AddItem(pChoice);
            }
            pPanel->AddItem(pRow);
            h += 26;
        }

        // --- wallpaper ----------------------------------------------------
        AddPopoverSection(this, pPanel, "壁纸");
        h += 1 + 8 + 4 + 20;
        {
            struct Wallpaper { const char* name; const char* colour; };
            const Wallpaper kWallpapers[] = {
                { "blue",   "#44A8F7" }, { "purple", "#7A5AC8" },
                { "dark",   "#1F2233" }, { "green",  "#35A87A" },
            };
            ui::HBox* pRow = new ui::HBox(this);
            pRow->SetAttribute("width", "stretch");
            pRow->SetAttribute("height", "24");
            for (const Wallpaper& w : kWallpapers) {
                ui::Button* pSwatch = MakeSwatch(this, w.colour,
                                                 m_settings.wallpaper == w.name);
                const std::string name = w.name;
                pSwatch->AttachClick([this, name](const ui::EventArgs&) {
                    pollux::SaveKey(pollux::kKeyWallpaper, name);
                    // The compositor paints the wallpaper, so it has to be told.
                    pollux::NotifyCompositor();
                    HideMenuPanel();
                    return true;
                });
                pRow->AddItem(pSwatch);
            }
            pPanel->AddItem(pRow);
            h += 24;
        }

        // --- sound --------------------------------------------------------
        AddPopoverSection(this, pPanel, "声音");
        h += 1 + 8 + 4 + 20;
        {
            int percent = ReadVolumePercent();
            if (percent < 0) {
                AddPopoverText(this, pPanel, "没有可用的混音器", g_pal.textHint,
                               "system_13");
            } else {
                ui::Slider* pSlider = new ui::Slider(this);
                pSlider->SetClass("slider_horizontal_green");
                pSlider->SetAttribute("width", "stretch");
                pSlider->SetAttribute("height", "26");
                pSlider->SetAttribute("thumb_size", "16,16");
                pSlider->SetAttribute("progress_color", g_pal.accent);
                pSlider->SetMinValue(0);
                pSlider->SetMaxValue(100);
                pSlider->SetValue(percent);
                pSlider->SetAttribute("cursor_type", "hand");
                pSlider->AttachValueChanged([](const ui::EventArgs& args) {
                    int value = static_cast<int>(args.wParam);
                    if (value < 0) value = 0;
                    if (value > 100) value = 100;
                    pollux::Run(("mixer vol=" + std::to_string(value) + "%").c_str());
                    return true;
                });
                pPanel->AddItem(pSlider);
            }
            h += 26;
        }

        // --- status -------------------------------------------------------
        AddPopoverSection(this, pPanel, "状态");
        h += 1 + 8 + 4 + 20;
        {
            const std::string ssid = CurrentSsid();
            AddPopoverText(this, pPanel,
                           U8String("Wi-Fi  ") +
                               (ssid.empty() ? U8String("未连接") : U8String(ssid.c_str())),
                           g_pal.textBody, "system_13");
            h += kPopoverRowH;

            const std::string life = pollux::Trim(pollux::Run("sysctl -n hw.acpi.battery.life 2>/dev/null"));
            AddPopoverText(this, pPanel,
                           U8String("电池  ") +
                               (life.empty() ? U8String("无") : U8String(life.c_str()) + "%") +
                               (AcOnline() ? "  已接通电源" : "  使用电池"),
                           g_pal.textBody, "system_13");
            h += kPopoverRowH;
        }

        // --- footer -------------------------------------------------------
        ui::Button* pOpen = new ui::Button(this);
        pOpen->SetText("打开系统设置…");
        pOpen->SetAttribute("font", "system_13");
        pOpen->SetAttribute("width", "stretch");
        pOpen->SetAttribute("height", "30");
        pOpen->SetAttribute("margin", "0,10,0,0");
        pOpen->SetAttribute("cursor_type", "hand");
        pOpen->SetStateColor(ui::kControlStateNormal, g_pal.accent);
        pOpen->SetStateColor(ui::kControlStateHot, g_pal.accent);
        pOpen->SetStateTextColor(ui::kControlStateNormal, "#FFFFFFFF");
        SetRadius(pOpen, 7, true);
        pOpen->AttachClick([this](const ui::EventArgs&) {
            HideMenuPanel();
            LaunchApp("\"" POLLUX_BIN "/polluxdesk_settings\"");
            return true;
        });
        pPanel->AddItem(pOpen);
        h += 40;
        return h;
    });
}

void PolluxOSForm::BuildDock(ui::VBox* pRoot)
{
    // Everything is derived from the configured icon size so the bar, the
    // tiles and their corners stay in proportion; the divisors are chosen so
    // the 36px default keeps the compact dock balanced.
    const int iconPx     = pollux::DockIconPx(m_settings);
    const int dotSize    = std::max(3, iconPx / 14);
    const int barHeight  = iconPx + 12; // 48 at the default, with 6px top/bottom
    const int barRadius  = iconPx * 18 / 56;   // 11 at the default
    const int iconGap    = 6;
    const int iconRadius = iconPx * 14 / 56;   // 9
    const int svgSize    = std::max(1, iconPx - 12); // 24, with 6px inset

    // macOS-style translucent icon dock (no labels), centered across the
    // bottom of the screen.
    ui::HBox* pDockRow = new ui::HBox(this);
    // Keep the existing six-pixel inset from the bottom edge.
    pDockRow->SetAttribute("height", Num(barHeight));
    pDockRow->SetAttribute("width", "stretch");
    pDockRow->SetAttribute("child_align", "hcenter,vcenter");
    pDockRow->SetAttribute("margin", "0,0,0,6");
    pRoot->AddItem(pDockRow);

    ui::HBox* pDock = new ui::HBox(this);
    m_pDockBar = pDock;
    pDock->SetAttribute("height", Num(barHeight));
    pDock->SetAttribute("width", "auto");
    pDock->SetAttribute("padding", "6,6,6,6");
    pDock->SetAttribute("child_align", "hcenter,vcenter");
    // The compositor supplies the translucent glass plate behind this client
    // surface so its alpha stays clear over every wallpaper/application.
    pDock->SetBkColor(g_pal.transparent);
    pDock->SetBkColor2(g_pal.transparent);
    pDock->SetBorderColor(g_pal.transparent);
    pDock->SetAttribute("border_size", "0");
    SetRadius(pDock, barRadius, false);
    pDockRow->AddItem(pDock);

    // A hairline between dock sections. macOS shows two of them -- one
    // between the everyday apps and the rest, one before the minimized
    // windows and the trash -- which is what gives the Dock its three groups.
    auto AddDockDivider = [this, pDock, iconPx, iconGap]() {
        ui::Control* pDivider = new ui::Control(this);
        pDivider->SetAttribute("width", "1");
        pDivider->SetAttribute("height", Num(iconPx * 62 / 100));
        pDivider->SetAttribute("margin",
            ui::StringUtil::Printf("%d,0,%d,0", iconGap + 3, iconGap + 3));
        pDivider->SetBkColor(g_pal.dockSeparator);
        pDivider->SetMouseEnabled(false);
        pDock->AddItem(pDivider);
    };

    m_dockDots.clear();
    m_dockDotAnchors.clear();
    int lastGroup = -1;
    const int kDockCount = static_cast<int>(sizeof(kDockApps) / sizeof(kDockApps[0]));
    for (int i = 0; i < kDockCount; ++i) {
        if (kDockApps[i].group != lastGroup) {
            if (lastGroup >= 0) {
                AddDockDivider();
            }
            lastGroup = kDockApps[i].group;
        }

        // One icon tile per app; the label is a tooltip, macOS style.
        ui::Button* pIcon = new ui::Button(this);
        pIcon->SetText(U8String(kDockApps[i].glyph));
        if (kDockApps[i].icon != nullptr) {
            pIcon->SetText("");
            pIcon->SetBkImage(U8String("file='") + kDockApps[i].icon +
                              "' width='" + Num(svgSize) + "' height='" +
                              Num(svgSize) + "' halign='center' valign='center'");
        }
        pIcon->SetAttribute("font", "system_bold_22");
        pIcon->SetStateTextColor(ui::kControlStateNormal, "#FFFFFFFF");
        pIcon->SetAttribute("text_align", "hcenter,vcenter");
        pIcon->SetAttribute("height", Num(iconPx));
        pIcon->SetAttribute("width", Num(iconPx));
        // The wrapper handles the spacing; the tile centres itself in it,
        // since a VBox only centres children that carry their own halign.
        pIcon->SetAttribute("halign", "center");
        pIcon->SetBkColor(U8String(kDockApps[i].color));
        pIcon->SetBkColor2(U8String(kDockApps[i].color2));
        pIcon->SetBkColor2Direction("1");   // left -> right gradient
        SetRadius(pIcon, iconRadius, true);
        pIcon->SetBorderColor(ui::kControlStateNormal, U8String(kDockApps[i].color2));
        pIcon->SetBorderColor(ui::kControlStateHot, g_pal.accent);
        pIcon->SetAttribute("cursor_type", "hand");
        pIcon->SetToolTipText(U8String(kDockApps[i].label));
        pIcon->AttachClick([this, i](const ui::EventArgs& /*args*/) {
            HideMenuPanel();
            if (kDockApps[i].cmd[0] == '\0') {
                ShowLaunchPad();   // the 启动台 tile opens the app grid
            } else {
                LaunchApp(kDockApps[i].cmd);
            }
            return true;
        });
        // Right-click: what is running under this tile, and the way to end it.
        pIcon->AttachRClick([this, i](const ui::EventArgs& args) {
            std::string pids;
            std::string title;
            for (const WindowInfo& info : m_windows) {
                if (!ExeListHas(kDockApps[i].exe, info.exe) || info.pid <= 0) {
                    continue;
                }
                if (!pids.empty()) {
                    pids += ' ';
                }
                pids += std::to_string(info.pid);
                if (title.empty()) {
                    title = info.title;
                }
            }
            if (title.empty()) {
                title = kDockApps[i].label;
            }
            ShowDockMenu(title, pids, args.ptMouse.x, args.ptMouse.y);
            return true;
        });

        ui::VBox* pItem = new ui::VBox(this);
        pItem->SetAttribute("width", Num(iconPx));
        pItem->SetAttribute("height", Num(iconPx));
        pItem->SetAttribute("margin", MarginH(iconGap));

        pItem->AddItem(pIcon);
        pDock->AddItem(pItem);

        // The indicator sits in the Dock's white lower inset, centered below
        // the tile rather than painted over the colored icon background.
        ui::Control* pDot = new ui::Control(this);
        pDot->SetFloat(true);
        pDot->SetKeepFloatPos(true);
        pDot->SetAttribute("width", Num(dotSize));
        pDot->SetAttribute("height", Num(dotSize));
        pDot->SetBkColor(g_pal.transparent);
        pDot->SetMouseEnabled(false);
        SetRadius(pDot, dotSize / 2, false);
        pDock->AddItem(pDot);
        m_dockDots.push_back(pDot);
        m_dockDotAnchors.push_back(pIcon);
    }

    // Programs running without a launcher of their own -- the Wi-Fi window and
    // Activity among them -- get a tile each, the way the Dock grows an icon
    // for an app that is open. A click brings that window forward; a
    // right-click offers to end it. The set is decided in UpdateRunningApps()
    // and the dock is rebuilt when it changes, so the tiles can be built here
    // at their final size like every other control in the bar.
    {
        int shown = 0;
        for (const WindowInfo& info : m_windows) {
            if (info.exe.empty() || HasPinnedTile(info.exe) ||
                    shown >= kRunningTileSlots) {
                continue;
            }
            ++shown;

            const RunningIcon* look = nullptr;
            for (int i = 0; i < kRunningIconCount; ++i) {
                if (info.exe == kRunningIcons[i].exe) {
                    look = &kRunningIcons[i];
                    break;
                }
            }
            const char* iconPath = look != nullptr
                                       ? look->icon
                                       : "polluxdesk/icons/app-generic.svg";
            const U8String colour(look != nullptr ? look->color : "#FF8E9AA6");
            const U8String colour2(look != nullptr ? look->color2 : "#FF6B7580");

            ui::Button* pIcon = new ui::Button(this);
            pIcon->SetBkImage(U8String("file='") + U8String(iconPath) +
                              "' width='" + Num(svgSize) + "' height='" +
                              Num(svgSize) + "' halign='center' valign='center'");
            pIcon->SetAttribute("height", Num(iconPx));
            pIcon->SetAttribute("width", Num(iconPx));
            pIcon->SetAttribute("halign", "center");
            pIcon->SetBkColor(colour);
            pIcon->SetBkColor2(colour2);
            pIcon->SetBkColor2Direction("1");
            SetRadius(pIcon, iconRadius, true);
            pIcon->SetBorderColor(ui::kControlStateNormal, colour2);
            pIcon->SetBorderColor(ui::kControlStateHot, g_pal.accent);
            pIcon->SetAttribute("cursor_type", "hand");
            pIcon->SetToolTipText(U8String(info.title.c_str()));

            const unsigned long id = info.id;
            pIcon->AttachClick([this, id](const ui::EventArgs& /*args*/) {
                HideMenuPanel();
                // Same channel the shelf uses to restore a minimized window:
                // the title is the only thing the compositor listens to, and
                // its one id-addressed command raises and focuses.
                RequestRestoreWindow(id);
                return true;
            });
            const std::string title = info.title.empty() ? info.exe : info.title;
            const std::string pids = std::to_string(info.pid);
            pIcon->AttachRClick([this, title, pids](const ui::EventArgs& args) {
                ShowDockMenu(title, pids, args.ptMouse.x, args.ptMouse.y);
                return true;
            });

            // Same-size wrapper as the pinned tiles; the running dot overlays
            // the lower edge so all tiles fit between the 6px bar insets.
            ui::VBox* pItem = new ui::VBox(this);
            pItem->SetAttribute("width", Num(iconPx));
            pItem->SetAttribute("height", Num(iconPx));
            pItem->SetAttribute("margin", MarginH(iconGap));
            pItem->AddItem(pIcon);
            pDock->AddItem(pItem);

            ui::Control* pDot = new ui::Control(this);
            pDot->SetFloat(true);
            pDot->SetKeepFloatPos(true);
            pDot->SetAttribute("width", Num(dotSize));
            pDot->SetAttribute("height", Num(dotSize));
            pDot->SetBkColor(g_pal.dockDot);
            pDot->SetMouseEnabled(false);
            SetRadius(pDot, dotSize / 2, false);
            pDock->AddItem(pDot);
            m_dockDots.push_back(pDot);
            m_dockDotAnchors.push_back(pIcon);
        }
    }

    // Right-hand side, inside the bar exactly as macOS has it: a hairline,
    // then the trash. The bar is centered by its full contents, so adding
    // these shifts the whole row half their width to the left -- which is
    // what the real Dock does too.
    AddDockDivider();

    // Minimized windows, between the separator and the trash as on macOS.
    // One chip per minimized window, created at its full size right here.
    // Resizing a control later does not make dui recompute its rect, and the
    // rect is what the hit-test uses -- a chip that was laid out at zero
    // width paints but can never be clicked. So the shelf is part of what a
    // rebuild produces rather than something patched afterwards.
    // Thumbnail-shaped, as the Dock's minimized windows are.
    const int chipH = iconPx;
    const int chipW = chipH * 8 / 5;
    m_minimizedSlots.clear();
    m_minimizedIds.clear();
    for (const WindowInfo& info : m_windows) {
        if (!info.minimized ||
                static_cast<int>(m_minimizedSlots.size()) >= kMinimizedSlots) {
            continue;
        }
        ui::Button* pChip = new ui::Button(this);
        pChip->SetAttribute("width", Num(chipW));
        pChip->SetAttribute("height", Num(chipH));
        pChip->SetAttribute("margin", MarginH(iconGap));
        // The dock centres a child by its own valign, not by the bar's
        // child_align (that one only covers the horizontal axis in HLayout).
        // Without this the chip sits against the top edge of the dock.
        pChip->SetAttribute("valign", "center");
        pChip->SetAttribute("font", "system_12");
        pChip->SetStateTextColor(ui::kControlStateNormal, g_pal.textBody);
        pChip->SetAttribute("text_align", "hcenter,vcenter");
        pChip->SetAttribute("cursor_type", "hand");
        pChip->SetBkColor(g_pal.chipBg);
        pChip->SetBorderColor(ui::kControlStateNormal, g_pal.chipBorder);
        pChip->SetBorderColor(ui::kControlStateHot, g_pal.accent);
        pChip->SetAttribute("border_size", "1");
        SetRadius(pChip, std::max(4, iconPx / 8), true);
        pChip->SetToolTipText(U8String(info.title.c_str()));

        const std::string shot = pollux::ThumbPath(info.id);
        if (pollux::FileExists(shot)) {
            // A picture of the window, taken while the compositor was still
            // holding it on screen. Scaled to the chip, which keeps the
            // window's proportions because grim grabbed its real rectangle.
            U8String spec = U8String("file='") + U8String(shot.c_str()) +
                "' width='" + Num(chipW - 4) + "' height='" +
                Num(chipH - 4) + "' halign='center' valign='center'";
            pChip->SetBkImage(spec);
        } else {
            // No picture (the grab failed, or did not happen): the title is
            // still worth showing, and dui ellipsizes it if it does not fit.
            pChip->SetText(U8String(info.title.c_str()));
        }

        const size_t index = m_minimizedIds.size();
        m_minimizedIds.push_back(info.id);
        pChip->AttachClick([this, index](const ui::EventArgs& /*args*/) {
            HideMenuPanel();
            if (index < m_minimizedIds.size() && m_minimizedIds[index] != 0) {
                // The shell's window title is the only channel it has to the
                // compositor; see the restore handling there. The marker is
                // cleared on the next tick so the two title changes cannot
                // coalesce into one commit.
                RequestRestoreWindow(m_minimizedIds[index]);
            }
            return true;
        });
        pDock->AddItem(pChip);
        m_minimizedSlots.push_back(pChip);
    }

    ui::ButtonVBox* pTrash = MakeTrashTile(this, iconPx);
    pTrash->SetAttribute("margin", MarginH(iconGap));
    pTrash->AttachClick([this](const ui::EventArgs& /*args*/) {
        HideMenuPanel();
        // The trash is an ordinary directory; the browser is simply pointed
        // at it. mkdir first so the very first open works on a fresh account.
        LaunchApp("mkdir -p \"$HOME/.Trash\"; exec " POLLUX_BIN
                  "/polluxdesk_files \"$HOME/.Trash\"");
        return true;
    });
    pDock->AddItem(pTrash);
}

void PolluxOSForm::StartClock()
{
    UpdateClock();
    // The repeat count matters: dui documents -1 as "repeat indefinitely", but
    // the test it re-arms on is `uRepeatTime > 0`, which -1 fails. Leaving the
    // default in place means the callback runs once and the timer is dropped --
    // which is why the menu-bar clock froze a second after startup and stayed
    // frozen. A large positive count is decremented once per fire and outlasts
    // any session.
    constexpr int32_t kRepeatForSession = 0x7FFFFFFF;
    m_clockTimerId = ui::GlobalManager::Instance().Timer().AddTimer(GetWeakFlag(), [this]() {
        UpdateClock();
    }, 250, kRepeatForSession);
}

void PolluxOSForm::UpdateClock()
{
    // Runs before the clock-label guard below so the dock keeps tracking
    // running apps even if no clock label was ever built.
    if (!m_menuOverlay) {
        PollWindowState();
    }

    if (m_titleMarkerPending) {
        // A tick after the request, so the compositor is certain to have seen
        // it as its own title change.
        m_titleMarkerPending = false;
        SetCompositorMenuOpen(false);
    }

    if (!m_dockOverlay && !m_menuOverlay) {
        GrabPendingThumbnail();
    }

    PollSettings();

    if (m_uiDirty) {
        m_uiDirty = false;
        RebuildUi();
    }

    UpdateDockHitArea();

    if (m_pClockLabel == nullptr && m_pDesktopClockLabel == nullptr &&
        m_pDesktopDateLabel == nullptr) {
        return;
    }
    std::time_t now = std::time(nullptr);
    std::tm tm_now;
    localtime_r(&now, &tm_now);

    const char* weekdays[] = { "周日", "周一", "周二", "周三", "周四", "周五", "周六" };
    char menuTimeBuf[80];
    std::snprintf(menuTimeBuf, sizeof(menuTimeBuf), "%s %d月%d日 %02d:%02d",
                  weekdays[tm_now.tm_wday],
                  tm_now.tm_mon + 1, tm_now.tm_mday,
                  tm_now.tm_hour, tm_now.tm_min);

    // The clock ticks four times a second so a minimize is not left waiting
    // on it; shelling out four times a second would be silly, so the reading
    // is only taken every few seconds.
    static int batteryTick = 0;
    if (m_pBatteryLabel != nullptr && ++batteryTick >= 20) {
        batteryTick = 0;
        std::string life = pollux::Run("sysctl -n hw.acpi.battery.life 2>/dev/null");
        while (!life.empty() && (life.back() == '\n' || life.back() == ' ')) {
            life.pop_back();
        }
        const U8String text = life.empty() ? U8String("--%")
                                          : U8String(life.c_str()) + "%";
        if (m_pBatteryLabel->GetText() != text) {
            m_pBatteryLabel->SetText(text);
        }
    }

    // The menu-bar clock shows minutes: skip SetText when the string did not
    // change, otherwise the timer invalidates the shell every second even
    // though the rendered text is identical.
    if (m_pClockLabel != nullptr && m_pClockLabel->GetText() != U8String(menuTimeBuf)) {
        m_pClockLabel->SetText(U8String(menuTimeBuf));
    }

    char desktopTimeBuf[32];
    std::snprintf(desktopTimeBuf, sizeof(desktopTimeBuf), "%02d:%02d:%02d",
                  tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    if (m_pDesktopClockLabel != nullptr) {
        m_pDesktopClockLabel->SetText(U8String(desktopTimeBuf));
    }

    char desktopDateBuf[80];
    std::snprintf(desktopDateBuf, sizeof(desktopDateBuf), "%04d年%02d月%02d日 %s",
                  tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                  weekdays[tm_now.tm_wday]);
    if (m_pDesktopDateLabel != nullptr) {
        m_pDesktopDateLabel->SetText(U8String(desktopDateBuf));
    }
}
