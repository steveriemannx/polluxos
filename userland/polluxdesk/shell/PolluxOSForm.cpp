#include "PolluxOSForm.h"
#include "PolluxPaths.h"

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
    DString barBg;          // frosted menu bar
    DString barBorder;      // hairline under the menu bar
    DString barHot;         // hovered menu-bar button
    DString menuPanelBg;    // frosted dropdown panel
    DString menuPanelLine;
    DString menuItemHot;
    DString dockBg;         // frosted dock
    DString dockBorder;
    DString dockSeparator;  // hairline before the trash
    DString dockDot;        // running-application indicator
    DString chipBg;         // minimized-window chip
    DString chipBorder;
    DString chipHot;
    DString textDark;
    DString textBody;
    DString textHint;
    DString accent;
    DString danger;
    DString transparent;
    // Which status-glyph set to load. dui cannot tint an SVG, so each glyph
    // ships in a dark and a light version and the shell picks one.
    DString glyphVariant;
    DString dockCanBody;    // trash-can fill, translucent like the reference
    DString dockCanEdge;
};

Palette g_pal;

// The accent at a lower alpha, for hover and selection washes. The settings
// module hands the accent over as "#AARRGGBB", so only the alpha pair moves.
DString AccentWash(const DString& accent, const char* alpha)
{
    if (accent.size() != 9 || accent[0] != DUI_T('#')) {
        return accent;
    }
    return DString(DUI_T("#")) + DString(alpha) + accent.substr(3);
}

void BuildPalette(const pollux::Settings& settings)
{
    const bool dark = pollux::IsDark(settings);
    const DString accent(pollux::AccentHex(settings));

    g_pal.transparent  = DUI_T("#00000000");
    g_pal.accent       = accent;
    g_pal.glyphVariant = dark ? DUI_T("white") : DUI_T("black");
    // A light can would vanish on the light dock and a dark one on the dark
    // dock, so the can follows the appearance the way macOS's does.
    g_pal.dockCanBody  = dark ? DUI_T("#D9F2F2F5") : DUI_T("#E6F7F7FA");
    g_pal.dockCanEdge  = dark ? DUI_T("#8CFFFFFF") : DUI_T("#5E000000");
    g_pal.danger       = DUI_T("#FFFF453A");
    // Same accent, washed out -- one source of truth for the highlight colour.
    g_pal.barHot       = AccentWash(accent, "22");
    g_pal.menuItemHot  = AccentWash(accent, "33");
    g_pal.chipHot      = AccentWash(accent, "33");

    if (dark) {
        g_pal.barBg        = DUI_T("#D91C1C1E");
        g_pal.barBorder    = DUI_T("#26FFFFFF");
        g_pal.menuPanelBg  = DUI_T("#F21C1C1E");
        g_pal.menuPanelLine= DUI_T("#26FFFFFF");
        g_pal.dockBg       = DUI_T("#B81C1C1E");
        g_pal.dockBorder   = DUI_T("#2EFFFFFF");
        g_pal.dockSeparator= DUI_T("#59FFFFFF");
        g_pal.dockDot      = DUI_T("#B3FFFFFF");
        g_pal.chipBg       = DUI_T("#26FFFFFF");
        g_pal.chipBorder   = DUI_T("#33FFFFFF");
        g_pal.textDark     = DUI_T("#FFF5F5F7");
        g_pal.textBody     = DUI_T("#FFD8D8DC");
        g_pal.textHint     = DUI_T("#FF98989D");
    } else {
        g_pal.barBg        = DUI_T("#F2FFFFFF");
        g_pal.barBorder    = DUI_T("#33000000");
        g_pal.menuPanelBg  = DUI_T("#F5FFFFFF");
        g_pal.menuPanelLine= DUI_T("#33000000");
        g_pal.dockBg       = DUI_T("#C9FFFFFF");
        g_pal.dockBorder   = DUI_T("#59FFFFFF");
        g_pal.dockSeparator= DUI_T("#3D000000");
        g_pal.dockDot      = DUI_T("#99000000");
        g_pal.chipBg       = DUI_T("#1A000000");
        g_pal.chipBorder   = DUI_T("#26000000");
        g_pal.textDark     = DUI_T("#FF1D1D1F");
        g_pal.textBody     = DUI_T("#FF3A3A3C");
        g_pal.textHint     = DUI_T("#FF8E8E93");
    }
}

// Number of fixed menu buttons: 文件 / 编辑 / 显示 / 前往 / 窗口 / 帮助.
const int kMenuButtonCount = 6;
const DString kMenuButtonText[kMenuButtonCount] = {
    DUI_T("文件"), DUI_T("编辑"), DUI_T("显示"), DUI_T("前往"), DUI_T("窗口"), DUI_T("帮助")
};

// Layout attributes are strings; this keeps the derived sizes readable.
static DString Num(int value)
{
    return ui::StringUtil::Printf(DUI_T("%d"), value);
}

const int kPopoverWidth = 300;
const int kPopoverPad   = 10;
const int kPopoverRowH  = 26;

// "left,top,right,bottom" with equal horizontal margins.
static DString MarginH(int px)
{
    return ui::StringUtil::Printf(DUI_T("%d,0,%d,0"), px, px);
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
    { "退出登录",       "/home/shxu/.local/bin/session-logout", false, true },
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
    { "退出登录",       "/home/shxu/.local/bin/session-logout", false, true },
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

PolluxOSForm::PolluxOSForm()
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

DString PolluxOSForm::GetSkinFolder()
{
    return DUI_T("");
}

DString PolluxOSForm::GetSkinFile()
{
    // Pure code mode: no layout XML is loaded
    return DUI_T("");
}

void PolluxOSForm::GetCreateWindowAttributes(ui::WindowCreateAttributes& attrs)
{
    attrs.m_bInitSizeDefined = true;
    attrs.m_szInitSize.cx = 1280;
    attrs.m_szInitSize.cy = 800;
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
    const std::string thumbClean =
        "rm -f '" + pollux::ThumbDir() + "'/*.png 2>/dev/null";
    pollux::Run(thumbClean.c_str());
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
    if (std::strcmp(cmdline, "/home/shxu/.local/bin/session-logout") == 0) {
        const char* home = std::getenv("HOME");
        DString launcher = DString(home != nullptr ? home : "/home/shxu") +
                           DUI_T("/projects-main/polluxos/build/polluxdesk/bin/launcher_code");
        pid_t pid = fork();
        if (pid < 0) {
            perror("[polluxdesk] logout fork");
            return;
        }
        if (pid == 0) {
            setsid();
            execl(launcher.c_str(), "launcher_code", static_cast<char*>(nullptr));
            perror("[polluxdesk] logout exec launcher_code");
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
    SetText(DUI_T("PolluxOS Desktop"));
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
            pSep->SetAttribute(DUI_T("height"), DUI_T("1"));
            pSep->SetAttribute(DUI_T("width"), DUI_T("stretch"));
            pSep->SetAttribute(DUI_T("margin"), DUI_T("8,4,8,4"));
            pSep->SetBkColor(g_pal.menuPanelLine);
            pSep->SetMouseEnabled(false);
            m_pMenuPanel->AddItem(pSep);
            continue;
        }

        panelHeight += kItemHeight;
        ui::Button* pItem = new ui::Button(this);
        pItem->SetText(DString(entry.text));
        pItem->SetAttribute(DUI_T("font"), DUI_T("system_14"));
        pItem->SetAttribute(DUI_T("text_align"), DUI_T("left,vcenter"));
        pItem->SetAttribute(DUI_T("text_padding"), DUI_T("10,0,10,0"));
        pItem->SetAttribute(DUI_T("height"), DUI_T("30"));
        pItem->SetAttribute(DUI_T("width"), DUI_T("218"));
        pItem->SetAttribute(DUI_T("margin"), DUI_T("0,0,0,0"));
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
        pItem->SetAttribute(DUI_T("cursor_type"), entry.enabled ? DUI_T("hand") : DUI_T("arrow"));

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

    m_pMenuPanel->SetAttribute(DUI_T("height"),
                               ui::StringUtil::Printf(DUI_T("%d"), panelHeight));

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
    SetText(DUI_T("PolluxOS Desktop (menu)"));
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
    pRoot->SetAttribute(DUI_T("border_size"), DUI_T("0"));
    pRoot->SetAttribute(DUI_T("padding"), DUI_T("0,0,0,0"));

    BuildMenuBar(pRoot);
    BuildDesktopArea(pRoot);
    BuildDock(pRoot);

    // Native in-window dropdown panel (shared by the desktop right-click
    // quick menu). It is created once, rebuilt per menu and positioned at the
    // click point.
    m_pMenuPanel = new ui::VBox(this);
    m_pMenuPanel->SetFloat(true);
    // Keep the explicitly positioned dropdown where ShowMenuPanel() put it.
    m_pMenuPanel->SetKeepFloatPos(true);
    m_pMenuPanel->SetBkColor(g_pal.menuPanelBg);
    m_pMenuPanel->SetBorderColor(g_pal.menuPanelLine);
    m_pMenuPanel->SetAttribute(DUI_T("border_size"), DUI_T("1"));
    m_pMenuPanel->SetAttribute(DUI_T("width"), DUI_T("230"));
    m_pMenuPanel->SetAttribute(DUI_T("padding"), DUI_T("6,6,6,6"));
    m_pMenuPanel->SetStateColorRound(ui::kControlStateNormal, ui::UiSize(10, 10), false);
    m_pMenuPanel->SetAttribute(DUI_T("border_round"), DUI_T("10,10"));
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
    m_pAppButton = nullptr;
    for (int i = 0; i < kMenuButtonCount; ++i) {
        m_menuButtons[i] = nullptr;
    }
    m_pClockLabel = nullptr;
    m_pDesktopClockLabel = nullptr;
    m_pDesktopDateLabel = nullptr;
    m_dockDots.clear();
    m_minimizedSlots.clear();
    m_minimizedIds.clear();

    BuildUi();
}

void PolluxOSForm::BuildMenuBar(ui::VBox* pRoot)
{
    ui::HBox* pTopBar = new ui::HBox(this);
    pTopBar->SetAttribute(DUI_T("height"), DUI_T("30"));
    pTopBar->SetBkColor(g_pal.barBg);
    pTopBar->SetBorderColor(g_pal.barBorder);
    pTopBar->SetAttribute(DUI_T("bottom_border_size"), DUI_T("1"));
    pTopBar->SetAttribute(DUI_T("padding"), DUI_T("12,0,12,0"));
    pRoot->AddItem(pTopBar);
    m_pMenuBar = pTopBar;

    // Bold app menu, macOS style (the Apple-menu slot).
    ui::Button* pAppButton = new ui::Button(this);
    pAppButton->SetText(DUI_T("PolluxOS"));
    m_pAppButton = pAppButton;
    pAppButton->SetAttribute(DUI_T("font"), DUI_T("system_bold_14"));
    pAppButton->SetStateTextColor(ui::kControlStateNormal, g_pal.textDark);
    pAppButton->SetAttribute(DUI_T("text_align"), DUI_T("left,vcenter"));
    pAppButton->SetAttribute(DUI_T("height"), DUI_T("24"));
    pAppButton->SetAttribute(DUI_T("width"), DUI_T("auto"));
    pAppButton->SetAttribute(DUI_T("margin"), DUI_T("0,3,4,3"));
    pAppButton->SetAttribute(DUI_T("text_padding"), DUI_T("6,0,6,0"));
    pAppButton->SetStateColor(ui::kControlStateNormal, g_pal.transparent);
    pAppButton->SetStateColor(ui::kControlStateHot, g_pal.barHot);
    pAppButton->SetStateColorRound(ui::kControlStateNormal, ui::UiSize(5, 5), false);
    pAppButton->SetStateColorRound(ui::kControlStateHot, ui::UiSize(5, 5), false);
    pAppButton->SetAttribute(DUI_T("cursor_type"), DUI_T("hand"));
    // macOS behavior: merely hovering the menu-bar title pops its menu open,
    // no click needed (click still works as a toggle).
    pAppButton->AttachMouseEnter([this, pAppButton](const ui::EventArgs& /*args*/) {
        if (m_pMenuPanel == nullptr || !m_pMenuPanel->IsVisible() ||
            m_openMenuIndex != -1) {
            HideMenuPanel();
            ui::UiRect rc = pAppButton->GetRect();
            ShowMenuPanel(kAppMenu, kMenuBarMenuCounts[0],
                          rc.IsEmpty() ? 12 : rc.left, rc.bottom + 2);
        }
        return true;
    });
    pAppButton->AttachClick([this, pAppButton](const ui::EventArgs& /*args*/) {
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
        pBtn->SetAttribute(DUI_T("font"), DUI_T("system_14"));
        pBtn->SetStateTextColor(ui::kControlStateNormal, g_pal.textDark);
        pBtn->SetAttribute(DUI_T("text_align"), DUI_T("left,vcenter"));
        pBtn->SetAttribute(DUI_T("height"), DUI_T("24"));
        pBtn->SetAttribute(DUI_T("width"), DUI_T("auto"));
        pBtn->SetAttribute(DUI_T("margin"), DUI_T("0,3,2,3"));
        pBtn->SetAttribute(DUI_T("text_padding"), DUI_T("6,0,6,0"));
        pBtn->SetStateColor(ui::kControlStateNormal, g_pal.transparent);
        pBtn->SetStateColor(ui::kControlStateHot, g_pal.barHot);
        pBtn->SetStateColorRound(ui::kControlStateNormal, ui::UiSize(5, 5), false);
        pBtn->SetStateColorRound(ui::kControlStateHot, ui::UiSize(5, 5), false);
        pBtn->SetAttribute(DUI_T("cursor_type"), DUI_T("hand"));
        // Hover-to-open: moving the pointer across the menu bar pops each
        // dropdown without a click (macOS-style menu tracking).
        pBtn->AttachMouseEnter([this, mi](const ui::EventArgs& /*args*/) {
            const bool alreadyOpen = m_pMenuPanel != nullptr &&
                                     m_pMenuPanel->IsVisible() &&
                                     m_openMenuIndex == mi;
            if (!alreadyOpen) {
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
    pTopSpacer->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    pTopSpacer->SetAttribute(DUI_T("mouse_enabled"), DUI_T("false"));
    pTopBar->AddItem(pTopSpacer);

    // Status glyphs, as macOS has them: small monochrome icons rather than
    // words. dui cannot tint an SVG, so each ships in a light and a dark
    // version and the palette says which to load.
    auto AddStatusGlyph = [this, pTopBar](const char* glyph) {
        ui::Button* pButton = new ui::Button(this);
        pButton->SetAttribute(DUI_T("width"), DUI_T("38"));
        pButton->SetAttribute(DUI_T("height"), DUI_T("24"));
        pButton->SetAttribute(DUI_T("cursor_type"), DUI_T("hand"));
        pButton->SetBkImage(DString(DUI_T("file='polluxdesk/icons/status_")) + glyph +
                            DUI_T("_") + g_pal.glyphVariant +
                            DUI_T(".svg' width='30' height='19' halign='center' valign='center'"));
        pButton->SetStateColor(ui::kControlStateNormal, g_pal.transparent);
        pButton->SetStateColor(ui::kControlStateHot, g_pal.barHot);
        pButton->SetStateColorRound(ui::kControlStateNormal, ui::UiSize(5, 5), false);
        pButton->SetStateColorRound(ui::kControlStateHot, ui::UiSize(5, 5), false);
        pButton->SetAttribute(DUI_T("border_round"), DUI_T("5,5"));
        pTopBar->AddItem(pButton);
        return pButton;
    };

    // The status items are buttons, not decoration: each one opens a panel of
    // real controls, as on macOS.
    ui::Button* pWifi = AddStatusGlyph("wifi");
    ui::Button* pVolume = AddStatusGlyph("volume");
    ui::Button* pBattery = AddStatusGlyph("battery");
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
    m_pBatteryLabel->SetAttribute(DUI_T("font"), DUI_T("system_12"));
    m_pBatteryLabel->SetStateTextColor(ui::kControlStateNormal, g_pal.textDark);
    m_pBatteryLabel->SetAttribute(DUI_T("text_align"), DUI_T("left,vcenter"));
    m_pBatteryLabel->SetAttribute(DUI_T("width"), DUI_T("42"));
    m_pBatteryLabel->SetAttribute(DUI_T("height"), DUI_T("24"));
    m_pBatteryLabel->SetText(DUI_T("--%"));
    m_pBatteryLabel->SetMouseEnabled(false);
    pTopBar->AddItem(m_pBatteryLabel);

    // Not a Spotlight field: the desktop shell never takes keyboard focus --
    // the compositor skips it deliberately so that clicking the wallpaper
    // does not steal the keyboard from the window in front -- so there is
    // nothing here that could receive typing. It opens the app grid instead.
    ui::Button* pSearch = AddStatusGlyph("search");
    pSearch->SetToolTipText(DUI_T("打开启动台"));
    pSearch->AttachClick([this](const ui::EventArgs&) {
        HideMenuPanel();
        ShowLaunchPad();
        return true;
    });

    ui::Button* controlCenter = new ui::Button(this);
    controlCenter->SetAttribute(DUI_T("height"), DUI_T("24"));
    controlCenter->SetAttribute(DUI_T("width"), DUI_T("40"));
    controlCenter->SetAttribute(DUI_T("margin"), DUI_T("4,3,4,3"));
    controlCenter->SetBkImage(DString(DUI_T("file='polluxdesk/icons/status_controlcenter_")) +
                              g_pal.glyphVariant +
                              DUI_T(".svg' width='27' height='17' halign='center' valign='center'"));
    controlCenter->SetStateColor(ui::kControlStateNormal, g_pal.transparent);
    controlCenter->SetStateColor(ui::kControlStateHot, g_pal.barHot);
    controlCenter->SetStateColorRound(ui::kControlStateNormal, ui::UiSize(5, 5), false);
    controlCenter->SetStateColorRound(ui::kControlStateHot, ui::UiSize(5, 5), false);
    controlCenter->SetAttribute(DUI_T("border_round"), DUI_T("5,5"));
    controlCenter->SetAttribute(DUI_T("cursor_type"), DUI_T("hand"));
    controlCenter->SetToolTipText(DUI_T("控制中心"));
    controlCenter->AttachClick([this, controlCenter](const ui::EventArgs&) {
        const ui::UiRect rc = controlCenter->GetRect();
        HideMenuPanel();
        ShowControlCentre(rc.right - kPopoverWidth, rc.bottom + 4);
        return true;
    });
    pTopBar->AddItem(controlCenter);

    // The clock is last, at the very right edge, as on macOS.
    m_pClockLabel = new ui::Label(this);
    m_pClockLabel->SetAttribute(DUI_T("font"), DUI_T("system_12"));
    m_pClockLabel->SetStateTextColor(ui::kControlStateNormal, g_pal.textDark);
    m_pClockLabel->SetAttribute(DUI_T("text_align"), DUI_T("right,vcenter"));
    m_pClockLabel->SetAttribute(DUI_T("width"), DUI_T("160"));
    m_pClockLabel->SetAttribute(DUI_T("margin"), DUI_T("0,0,8,0"));
    m_pClockLabel->SetText(DUI_T("--月--日 周- --:--"));
    m_pClockLabel->SetMouseEnabled(false);
    pTopBar->AddItem(m_pClockLabel);
}

void PolluxOSForm::BuildDesktopArea(ui::VBox* pRoot)
{
    // The desktop area is just the wallpaper: no top bar, no clock/date in the
    // middle. It only provides the desktop right-click quick menu and dismisses
    // any open dropdown on a plain left click.
    ui::VBox* pDesktop = new ui::VBox(this);
    pDesktop->SetAttribute(DUI_T("height"), DUI_T("stretch"));
    pDesktop->SetAttribute(DUI_T("mouse_enabled"), DUI_T("true"));
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

    SetText(ui::StringUtil::Printf(DUI_T("PolluxOS Desktop (thumb-ready:%lu)"), id));
    m_titleMarkerPending = true;
    m_uiDirty = true;   // the shelf can show the picture on the next build
}

bool PolluxOSForm::IsAppRunning(const char* exeList) const
{
    if (exeList == nullptr || exeList[0] == '\0') {
        return false;   // this tile never shows a dot
    }
    const std::string list(exeList);
    for (const WindowInfo& info : m_windows) {
        if (info.exe.empty()) {
            continue;
        }
        // "|"-separated so one tile can stand for any of several programs --
        // the browser tile covers whichever of firefox/chromium got installed.
        size_t start = 0;
        while (start <= list.size()) {
            size_t end = list.find('|', start);
            if (end == std::string::npos) {
                end = list.size();
            }
            if (list.compare(start, end - start, info.exe) == 0) {
                return true;
            }
            start = end + 1;
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
    pControl->SetAttribute(DUI_T("border_round"),
        ui::StringUtil::Printf(DUI_T("%d,%d"), radius, radius));
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
    const DString canBody   = g_pal.dockCanBody;
    const DString canEdge   = g_pal.dockCanEdge;

    ui::ButtonVBox* pTile = new ui::ButtonVBox(pWindow);
    pTile->SetAttribute(DUI_T("height"), Num(size));
    pTile->SetAttribute(DUI_T("width"), Num(size));
    pTile->SetBkColor(g_pal.transparent);
    pTile->SetBorderColor(ui::kControlStateNormal, g_pal.transparent);
    pTile->SetBorderColor(ui::kControlStateHot, g_pal.accent);
    pTile->SetAttribute(DUI_T("cursor_type"), DUI_T("hand"));
    pTile->SetToolTipText(DUI_T("废纸篓"));
    SetRadius(pTile, scale(14, 56), true);

    // dui splits centring in a VBox between the layout and the child:
    // child_align="vcenter" offsets the whole run of children vertically, but
    // there is no horizontal equivalent, so each part carries its own
    // halign="center". With only the layout attribute the can sits against
    // the tile's left edge.
    pTile->SetAttribute(DUI_T("child_align"), DUI_T("vcenter"));

    // Lid handle, lid, then the body: stacked vertically so the can grows
    // from the top down as the icon size changes.
    const int handleH = std::max(2, scale(3, 56));
    const int lidH    = std::max(3, scale(4, 56));
    const int bodyW   = scale(22, 56);
    const int bodyH   = scale(26, 56);

    ui::Control* pHandle = new ui::Control(pWindow);
    pHandle->SetAttribute(DUI_T("halign"), DUI_T("center"));
    pHandle->SetAttribute(DUI_T("width"), Num(scale(10, 56)));
    pHandle->SetAttribute(DUI_T("height"), Num(handleH));
    pHandle->SetBkColor(canBody);
    pHandle->SetMouseEnabled(false);
    SetRadius(pHandle, std::max(1, handleH / 2), false);
    pTile->AddItem(pHandle);

    ui::Control* pLid = new ui::Control(pWindow);
    pLid->SetAttribute(DUI_T("halign"), DUI_T("center"));
    pLid->SetAttribute(DUI_T("width"), Num(scale(26, 56)));
    pLid->SetAttribute(DUI_T("height"), Num(lidH));
    pLid->SetBkColor(canBody);
    pLid->SetBorderColor(canEdge);
    pLid->SetAttribute(DUI_T("border_size"), DUI_T("1"));
    pLid->SetMouseEnabled(false);
    SetRadius(pLid, std::max(1, lidH / 2), false);
    pTile->AddItem(pLid);

    ui::Control* pGap = new ui::Control(pWindow);
    pGap->SetAttribute(DUI_T("halign"), DUI_T("center"));
    pGap->SetAttribute(DUI_T("height"), Num(std::max(1, scale(2, 56))));
    pGap->SetMouseEnabled(false);
    pTile->AddItem(pGap);

    // HBox so the three ribs stand upright.
    ui::HBox* pBody = new ui::HBox(pWindow);
    pBody->SetAttribute(DUI_T("halign"), DUI_T("center"));
    pBody->SetAttribute(DUI_T("width"), Num(bodyW));
    pBody->SetAttribute(DUI_T("height"), Num(bodyH));
    pBody->SetBkColor(canBody);
    pBody->SetBkColor2(canEdge);
    pBody->SetBkColor2Direction(DUI_T("1"));
    pBody->SetBorderColor(canEdge);
    pBody->SetAttribute(DUI_T("border_size"), DUI_T("1"));
    pBody->SetAttribute(DUI_T("child_align"), DUI_T("hcenter,vcenter"));
    pBody->SetMouseEnabled(false);
    SetRadius(pBody, std::max(2, scale(4, 56)), false);

    for (int i = 0; i < 3; ++i) {
        ui::Control* pRib = new ui::Control(pWindow);
        pRib->SetAttribute(DUI_T("width"), DUI_T("1"));
        pRib->SetAttribute(DUI_T("height"), Num(std::max(4, bodyH - scale(8, 56))));
        pRib->SetAttribute(DUI_T("margin"), MarginH(std::max(1, scale(2, 56))));
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
                                 const DString& text, const DString& colour,
                                 const DString& font)
{
    ui::Label* pLabel = new ui::Label(pWindow);
    pLabel->SetText(text);
    pLabel->SetAttribute(DUI_T("font"), font);
    pLabel->SetStateTextColor(ui::kControlStateNormal, colour);
    pLabel->SetAttribute(DUI_T("text_align"), DUI_T("left,vcenter"));
    pLabel->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    pLabel->SetAttribute(DUI_T("height"), Num(kPopoverRowH));
    pLabel->SetMouseEnabled(false);
    pPanel->AddItem(pLabel);
    return pLabel;
}

// A section divider with a dim heading, which is how macOS groups a popover.
static void AddPopoverSection(ui::Window* pWindow, ui::VBox* pPanel,
                              const DString& text)
{
    ui::Control* pLine = new ui::Control(pWindow);
    pLine->SetAttribute(DUI_T("height"), DUI_T("1"));
    pLine->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    pLine->SetAttribute(DUI_T("margin"), DUI_T("0,8,0,4"));
    pLine->SetBkColor(g_pal.menuPanelLine);
    pLine->SetMouseEnabled(false);
    pPanel->AddItem(pLine);

    ui::Label* pTitle = AddPopoverText(pWindow, pPanel, text, g_pal.textHint,
                                       DUI_T("system_12"));
    pTitle->SetAttribute(DUI_T("height"), DUI_T("20"));
}

// A round-ish chip used for the accent and wallpaper swatches.
static ui::Button* MakeSwatch(ui::Window* pWindow, const DString& colour,
                              bool selected)
{
    ui::Button* pSwatch = new ui::Button(pWindow);
    pSwatch->SetAttribute(DUI_T("width"), DUI_T("34"));
    pSwatch->SetAttribute(DUI_T("height"), DUI_T("24"));
    pSwatch->SetAttribute(DUI_T("margin"), DUI_T("0,0,8,0"));
    pSwatch->SetAttribute(DUI_T("cursor_type"), DUI_T("hand"));
    pSwatch->SetStateColor(ui::kControlStateNormal, colour);
    pSwatch->SetStateColor(ui::kControlStateHot, colour);
    pSwatch->SetBorderColor(ui::kControlStateNormal,
                            selected ? g_pal.accent : g_pal.chipBorder);
    pSwatch->SetBorderColor(ui::kControlStateHot, g_pal.accent);
    pSwatch->SetAttribute(DUI_T("border_size"), DUI_T("2"));
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

// Access points the interface can see. `ifconfig wlan0 list scan` needs no
// root on this system, so the list is real rather than a mock.
static void ScanWireless(std::vector<std::string>* ssids)
{
    const std::string out = pollux::Run("ifconfig wlan0 list scan 2>/dev/null");
    size_t pos = 0;
    bool first = true;
    while (pos < out.size()) {
        size_t end = out.find('\n', pos);
        if (end == std::string::npos) {
            end = out.size();
        }
        const std::string line = pollux::Trim(out.substr(pos, end - pos));
        pos = end + 1;
        if (first) {
            first = false;   // the column headings
            continue;
        }
        if (line.empty()) {
            continue;
        }
        const size_t space = line.find(' ');
        const std::string ssid = space == std::string::npos
            ? line : line.substr(0, space);
        bool seen = false;
        for (const std::string& known : *ssids) {
            if (known == ssid) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            ssids->push_back(ssid);
        }
    }
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

    m_pMenuPanel->SetAttribute(DUI_T("width"), Num(width));
    m_pMenuPanel->SetAttribute(DUI_T("height"), Num(height));
    m_pMenuPanel->SetPos(ui::UiRect(px, py, px + width, py + height));
    m_pMenuPanel->SetPaintOrder(100);
    m_pMenuPanel->SetVisible(true);
    // Raise the shell above app windows for as long as the panel is open.
    SetText(DUI_T("PolluxOS Desktop (menu)"));
    Invalidate(m_pMenuPanel->GetPos());
}

void PolluxOSForm::ShowVolumePanel(int x, int y)
{
    ShowPopover(x, y, kPopoverWidth, [this](ui::VBox* pPanel) -> int {
        int h = kPopoverPad * 2;
        AddPopoverText(this, pPanel, DUI_T("声音"), g_pal.textDark, DUI_T("system_15"));
        h += kPopoverRowH;

        int percent = ReadVolumePercent();
        if (percent < 0) {
            AddPopoverText(this, pPanel, DUI_T("这台机器没有可用的混音器"),
                           g_pal.textHint, DUI_T("system_13"));
            return h + kPopoverRowH;
        }

        // A slider and its reading, side by side.
        ui::HBox* pRow = new ui::HBox(this);
        pRow->SetAttribute(DUI_T("width"), DUI_T("stretch"));
        pRow->SetAttribute(DUI_T("height"), DUI_T("26"));

        ui::Slider* pSlider = new ui::Slider(this);
        pSlider->SetClass(DUI_T("slider_horizontal_green"));
        pSlider->SetAttribute(DUI_T("width"), DUI_T("stretch"));
        pSlider->SetAttribute(DUI_T("height"), DUI_T("26"));
        pSlider->SetAttribute(DUI_T("thumb_size"), DUI_T("16,16"));
        pSlider->SetAttribute(DUI_T("progress_color"), g_pal.accent);
        pSlider->SetMinValue(0);
        pSlider->SetMaxValue(100);
        pSlider->SetValue(percent);
        pSlider->SetAttribute(DUI_T("cursor_type"), DUI_T("hand"));
        ui::Label* pValue = new ui::Label(this);
        pValue->SetAttribute(DUI_T("font"), DUI_T("system_12"));
        pValue->SetStateTextColor(ui::kControlStateNormal, g_pal.textBody);
        pValue->SetAttribute(DUI_T("text_align"), DUI_T("right,vcenter"));
        pValue->SetAttribute(DUI_T("width"), DUI_T("44"));
        pValue->SetText(Num(percent) + DUI_T("%"));

        pSlider->AttachValueChanged([this, pValue](const ui::EventArgs& args) {
            int value = static_cast<int>(args.wParam);
            if (value < 0) value = 0;
            if (value > 100) value = 100;
            pValue->SetText(Num(value) + DUI_T("%"));
            pollux::Run(("mixer vol=" + std::to_string(value) + "%").c_str());
            return true;
        });

        pRow->AddItem(pSlider);
        pRow->AddItem(pValue);
        pPanel->AddItem(pRow);
        h += 26;

        ui::CheckBox* pMute = new ui::CheckBox(this);
        pMute->SetText(DUI_T("静音"));
        pMute->SetAttribute(DUI_T("font"), DUI_T("system_13"));
        pMute->SetStateTextColor(ui::kControlStateNormal, g_pal.textBody);
        pMute->SetAttribute(DUI_T("height"), Num(kPopoverRowH));
        pMute->SetAttribute(DUI_T("cursor_type"), DUI_T("hand"));
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
        AddPopoverText(this, pPanel, DUI_T("Wi-Fi"), g_pal.textDark, DUI_T("system_15"));
        h += kPopoverRowH;

        const std::string ssid = CurrentSsid();
        AddPopoverText(this, pPanel,
                       ssid.empty() ? DString(DUI_T("未连接")) : DString(ssid.c_str()),
                       ssid.empty() ? g_pal.textHint : g_pal.textBody,
                       DUI_T("system_13"));
        h += kPopoverRowH;

        std::vector<std::string> found;
        ScanWireless(&found);
        AddPopoverSection(this, pPanel, DUI_T("扫描到的网络"));
        h += 1 + 8 + 4 + 20;   // the divider and its margins, then the heading

        if (found.empty()) {
            AddPopoverText(this, pPanel, DUI_T("没有扫描到网络"), g_pal.textHint,
                           DUI_T("system_13"));
            h += kPopoverRowH;
        }
        for (size_t i = 0; i < found.size() && i < 8; ++i) {
            AddPopoverText(this, pPanel, DString(found[i].c_str()), g_pal.textBody,
                           DUI_T("system_13"));
            h += kPopoverRowH;
        }

        AddPopoverSection(this, pPanel, DUI_T(""));
        h += 1 + 8 + 4 + 20;
        AddPopoverText(this, pPanel,
                       DUI_T("连接网络需要 root 权限，这里只列出扫描结果"),
                       g_pal.textHint, DUI_T("system_12"));
        h += kPopoverRowH;
        return h;
    });
}

void PolluxOSForm::ShowBatteryPanel(int x, int y)
{
    ShowPopover(x, y, kPopoverWidth, [this](ui::VBox* pPanel) -> int {
        int h = kPopoverPad * 2;
        AddPopoverText(this, pPanel, DUI_T("电池"), g_pal.textDark, DUI_T("system_15"));
        h += kPopoverRowH;

        std::string life = pollux::Trim(pollux::Run("sysctl -n hw.acpi.battery.life 2>/dev/null"));
        std::string time = pollux::Trim(pollux::Run("sysctl -n hw.acpi.battery.time 2>/dev/null"));
        const bool online = AcOnline();

        if (life.empty()) {
            AddPopoverText(this, pPanel, DUI_T("这台机器没有电池"), g_pal.textHint,
                           DUI_T("system_13"));
            return h + kPopoverRowH;
        }

        AddPopoverText(this, pPanel, DString(life.c_str()) + DUI_T("%"),
                       g_pal.textDark, DUI_T("system_20"));
        h += kPopoverRowH + 6;

        const int minutes = std::atoi(time.c_str());
        if (minutes > 0) {
            AddPopoverText(this, pPanel,
                           online ? DString(DUI_T("充满还需 ")) + Num(minutes) + DUI_T(" 分钟")
                                  : DString(DUI_T("剩余 ")) + Num(minutes) + DUI_T(" 分钟"),
                           g_pal.textBody, DUI_T("system_13"));
            h += kPopoverRowH;
        }
        AddPopoverText(this, pPanel,
                       online ? DUI_T("已接通电源") : DUI_T("使用电池供电"),
                       g_pal.textHint, DUI_T("system_13"));
        return h + kPopoverRowH;
    });
}

void PolluxOSForm::ShowControlCentre(int x, int y)
{
    ShowPopover(x, y, kPopoverWidth, [this](ui::VBox* pPanel) -> int {
        int h = kPopoverPad * 2;
        AddPopoverText(this, pPanel, DUI_T("控制中心"), g_pal.textDark, DUI_T("system_15"));
        h += kPopoverRowH;

        // --- appearance ---------------------------------------------------
        AddPopoverSection(this, pPanel, DUI_T("外观"));
        h += 1 + 8 + 4 + 20;
        {
            ui::HBox* pRow = new ui::HBox(this);
            pRow->SetAttribute(DUI_T("width"), DUI_T("stretch"));
            pRow->SetAttribute(DUI_T("height"), DUI_T("28"));
            const char* kLabels[] = { "浅色", "深色", "自动" };
            const char* kValues[] = { "light", "dark", "auto" };
            for (int i = 0; i < 3; ++i) {
                ui::Button* pChoice = new ui::Button(this);
                pChoice->SetText(DString(kLabels[i]));
                pChoice->SetAttribute(DUI_T("font"), DUI_T("system_13"));
                pChoice->SetAttribute(DUI_T("width"), DUI_T("stretch"));
                pChoice->SetAttribute(DUI_T("height"), DUI_T("28"));
                pChoice->SetAttribute(DUI_T("margin"), DUI_T("0,0,6,0"));
                pChoice->SetAttribute(DUI_T("cursor_type"), DUI_T("hand"));
                const bool active = m_settings.appearance == kValues[i];
                pChoice->SetStateColor(ui::kControlStateNormal,
                                       active ? g_pal.accent : g_pal.chipBg);
                pChoice->SetStateColor(ui::kControlStateHot, g_pal.chipHot);
                pChoice->SetStateTextColor(ui::kControlStateNormal,
                                           active ? DUI_T("#FFFFFFFF") : g_pal.textBody);
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
        AddPopoverSection(this, pPanel, DUI_T("壁纸"));
        h += 1 + 8 + 4 + 20;
        {
            struct Wallpaper { const char* name; const char* colour; };
            const Wallpaper kWallpapers[] = {
                { "blue",   "#44A8F7" }, { "purple", "#7A5AC8" },
                { "dark",   "#1F2233" }, { "green",  "#35A87A" },
            };
            ui::HBox* pRow = new ui::HBox(this);
            pRow->SetAttribute(DUI_T("width"), DUI_T("stretch"));
            pRow->SetAttribute(DUI_T("height"), DUI_T("24"));
            for (const Wallpaper& w : kWallpapers) {
                ui::Button* pSwatch = MakeSwatch(this, DUI_T(w.colour),
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
        AddPopoverSection(this, pPanel, DUI_T("声音"));
        h += 1 + 8 + 4 + 20;
        {
            int percent = ReadVolumePercent();
            if (percent < 0) {
                AddPopoverText(this, pPanel, DUI_T("没有可用的混音器"), g_pal.textHint,
                               DUI_T("system_13"));
            } else {
                ui::Slider* pSlider = new ui::Slider(this);
                pSlider->SetClass(DUI_T("slider_horizontal_green"));
                pSlider->SetAttribute(DUI_T("width"), DUI_T("stretch"));
                pSlider->SetAttribute(DUI_T("height"), DUI_T("26"));
                pSlider->SetAttribute(DUI_T("thumb_size"), DUI_T("16,16"));
                pSlider->SetAttribute(DUI_T("progress_color"), g_pal.accent);
                pSlider->SetMinValue(0);
                pSlider->SetMaxValue(100);
                pSlider->SetValue(percent);
                pSlider->SetAttribute(DUI_T("cursor_type"), DUI_T("hand"));
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
        AddPopoverSection(this, pPanel, DUI_T("状态"));
        h += 1 + 8 + 4 + 20;
        {
            const std::string ssid = CurrentSsid();
            AddPopoverText(this, pPanel,
                           DString(DUI_T("Wi-Fi  ")) +
                               (ssid.empty() ? DString(DUI_T("未连接")) : DString(ssid.c_str())),
                           g_pal.textBody, DUI_T("system_13"));
            h += kPopoverRowH;

            const std::string life = pollux::Trim(pollux::Run("sysctl -n hw.acpi.battery.life 2>/dev/null"));
            AddPopoverText(this, pPanel,
                           DString(DUI_T("电池  ")) +
                               (life.empty() ? DString(DUI_T("无")) : DString(life.c_str()) + DUI_T("%")) +
                               (AcOnline() ? DUI_T("  已接通电源") : DUI_T("  使用电池")),
                           g_pal.textBody, DUI_T("system_13"));
            h += kPopoverRowH;
        }

        // --- footer -------------------------------------------------------
        ui::Button* pOpen = new ui::Button(this);
        pOpen->SetText(DUI_T("打开系统设置…"));
        pOpen->SetAttribute(DUI_T("font"), DUI_T("system_13"));
        pOpen->SetAttribute(DUI_T("width"), DUI_T("stretch"));
        pOpen->SetAttribute(DUI_T("height"), DUI_T("30"));
        pOpen->SetAttribute(DUI_T("margin"), DUI_T("0,10,0,0"));
        pOpen->SetAttribute(DUI_T("cursor_type"), DUI_T("hand"));
        pOpen->SetStateColor(ui::kControlStateNormal, g_pal.accent);
        pOpen->SetStateColor(ui::kControlStateHot, g_pal.accent);
        pOpen->SetStateTextColor(ui::kControlStateNormal, DUI_T("#FFFFFFFF"));
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
    // the 56px default reproduces the original hard-coded design.
    const int iconPx     = pollux::DockIconPx(m_settings);
    const int barHeight  = iconPx + 20;        // 76 at the default
    const int barRadius  = iconPx * 18 / 56;   // 18
    const int iconGap    = iconPx * 4 / 56;    // 4
    const int iconRadius = iconPx * 14 / 56;   // 14
    const int svgSize    = iconPx * 48 / 56;   // 48

    // macOS-style centered translucent icon dock (no labels, like the real
    // Dock). The dock is wrapped in a full-width HBox with
    // child_align="hcenter,vcenter": that reliably centers the auto-width
    // frosted bar on the screen.
    ui::HBox* pDockRow = new ui::HBox(this);
    pDockRow->SetAttribute(DUI_T("height"), DUI_T("92"));
    pDockRow->SetAttribute(DUI_T("child_align"), DUI_T("hcenter,vcenter"));
    pDockRow->SetAttribute(DUI_T("margin"), DUI_T("0,0,0,14"));
    pRoot->AddItem(pDockRow);

    ui::HBox* pDock = new ui::HBox(this);
    pDock->SetAttribute(DUI_T("height"), Num(barHeight));
    pDock->SetAttribute(DUI_T("width"), DUI_T("auto"));
    pDock->SetAttribute(DUI_T("padding"), DUI_T("6,6,6,6"));
    pDock->SetAttribute(DUI_T("child_align"), DUI_T("hcenter,vcenter"));
    pDock->SetBkColor(g_pal.dockBg);
    pDock->SetBorderColor(g_pal.dockBorder);
    pDock->SetAttribute(DUI_T("border_size"), DUI_T("1"));
    SetRadius(pDock, barRadius, false);
    pDockRow->AddItem(pDock);

    // Room under each tile for the running-app dot. The lane is reserved
    // whether or not anything is running, so tiles never shift as apps start
    // and stop -- the dot only changes colour.
    const int dotLaneH = std::max(5, iconPx / 8);
    const int dotSize  = std::max(3, iconPx / 14);

    // A hairline between dock sections. macOS shows two of them -- one
    // between the everyday apps and the rest, one before the minimized
    // windows and the trash -- which is what gives the Dock its three groups.
    auto AddDockDivider = [this, pDock, iconPx, iconGap]() {
        ui::Control* pDivider = new ui::Control(this);
        pDivider->SetAttribute(DUI_T("width"), DUI_T("1"));
        pDivider->SetAttribute(DUI_T("height"), Num(iconPx * 62 / 100));
        pDivider->SetAttribute(DUI_T("margin"),
            ui::StringUtil::Printf(DUI_T("%d,0,%d,0"), iconGap + 3, iconGap + 3));
        pDivider->SetBkColor(g_pal.dockSeparator);
        pDivider->SetMouseEnabled(false);
        pDock->AddItem(pDivider);
    };

    m_dockDots.clear();
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
        pIcon->SetText(DString(kDockApps[i].glyph));
        if (kDockApps[i].icon != nullptr) {
            pIcon->SetText(DUI_T(""));
            pIcon->SetBkImage(DString(DUI_T("file='")) + kDockApps[i].icon +
                              DUI_T("' width='") + Num(svgSize) + DUI_T("' height='") +
                              Num(svgSize) + DUI_T("' halign='center' valign='center'"));
        }
        pIcon->SetAttribute(DUI_T("font"), DUI_T("system_bold_22"));
        pIcon->SetStateTextColor(ui::kControlStateNormal, DUI_T("#FFFFFFFF"));
        pIcon->SetAttribute(DUI_T("text_align"), DUI_T("hcenter,vcenter"));
        pIcon->SetAttribute(DUI_T("height"), Num(iconPx));
        pIcon->SetAttribute(DUI_T("width"), Num(iconPx));
        // The wrapper handles the spacing; the tile centres itself in it,
        // since a VBox only centres children that carry their own halign.
        pIcon->SetAttribute(DUI_T("halign"), DUI_T("center"));
        pIcon->SetBkColor(DString(kDockApps[i].color));
        pIcon->SetBkColor2(DString(kDockApps[i].color2));
        pIcon->SetBkColor2Direction(DUI_T("1"));   // left -> right gradient
        SetRadius(pIcon, iconRadius, true);
        pIcon->SetBorderColor(ui::kControlStateNormal, DString(kDockApps[i].color2));
        pIcon->SetBorderColor(ui::kControlStateHot, g_pal.accent);
        pIcon->SetAttribute(DUI_T("cursor_type"), DUI_T("hand"));
        pIcon->SetToolTipText(DString(kDockApps[i].label));
        pIcon->AttachClick([this, i](const ui::EventArgs& /*args*/) {
            HideMenuPanel();
            if (kDockApps[i].cmd[0] == '\0') {
                ShowLaunchPad();   // the 启动台 tile opens the app grid
            } else {
                LaunchApp(kDockApps[i].cmd);
            }
            return true;
        });

        ui::VBox* pItem = new ui::VBox(this);
        pItem->SetAttribute(DUI_T("width"), Num(iconPx));
        pItem->SetAttribute(DUI_T("height"), Num(iconPx + dotLaneH));
        pItem->SetAttribute(DUI_T("margin"), MarginH(iconGap));

        pItem->AddItem(pIcon);

        ui::Control* pDot = new ui::Control(this);
        pDot->SetAttribute(DUI_T("width"), Num(dotSize));
        pDot->SetAttribute(DUI_T("height"), Num(dotSize));
        pDot->SetAttribute(DUI_T("halign"), DUI_T("center"));
        pDot->SetAttribute(DUI_T("margin"),
            ui::StringUtil::Printf(DUI_T("0,%d,0,0"), std::max(1, dotLaneH - dotSize - 2)));
        pDot->SetBkColor(g_pal.transparent);
        pDot->SetMouseEnabled(false);
        SetRadius(pDot, dotSize / 2, false);
        pItem->AddItem(pDot);
        m_dockDots.push_back(pDot);

        pDock->AddItem(pItem);
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
    const int chipH = iconPx + 4;
    const int chipW = chipH * 8 / 5;
    m_minimizedSlots.clear();
    m_minimizedIds.clear();
    for (const WindowInfo& info : m_windows) {
        if (!info.minimized ||
                static_cast<int>(m_minimizedSlots.size()) >= kMinimizedSlots) {
            continue;
        }
        ui::Button* pChip = new ui::Button(this);
        pChip->SetAttribute(DUI_T("width"), Num(chipW));
        pChip->SetAttribute(DUI_T("height"), Num(chipH));
        pChip->SetAttribute(DUI_T("margin"), MarginH(iconGap / 2));
        // The dock centres a child by its own valign, not by the bar's
        // child_align (that one only covers the horizontal axis in HLayout).
        // Without this the chip sits against the top edge of the dock.
        pChip->SetAttribute(DUI_T("valign"), DUI_T("center"));
        pChip->SetAttribute(DUI_T("font"), DUI_T("system_12"));
        pChip->SetStateTextColor(ui::kControlStateNormal, g_pal.textBody);
        pChip->SetAttribute(DUI_T("text_align"), DUI_T("hcenter,vcenter"));
        pChip->SetAttribute(DUI_T("cursor_type"), DUI_T("hand"));
        pChip->SetBkColor(g_pal.chipBg);
        pChip->SetBorderColor(ui::kControlStateNormal, g_pal.chipBorder);
        pChip->SetBorderColor(ui::kControlStateHot, g_pal.accent);
        pChip->SetAttribute(DUI_T("border_size"), DUI_T("1"));
        SetRadius(pChip, std::max(4, iconPx / 8), true);
        pChip->SetToolTipText(DString(info.title.c_str()));

        const std::string shot = pollux::ThumbPath(info.id);
        if (pollux::FileExists(shot)) {
            // A picture of the window, taken while the compositor was still
            // holding it on screen. Scaled to the chip, which keeps the
            // window's proportions because grim grabbed its real rectangle.
            DString spec = DString(DUI_T("file='")) + DString(shot.c_str()) +
                DUI_T("' width='") + Num(chipW - 4) + DUI_T("' height='") +
                Num(chipH - 4) + DUI_T("' halign='center' valign='center'");
            pChip->SetBkImage(spec);
        } else {
            // No picture (the grab failed, or did not happen): the title is
            // still worth showing, and dui ellipsizes it if it does not fit.
            pChip->SetText(DString(info.title.c_str()));
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
                SetText(ui::StringUtil::Printf(
                    DUI_T("PolluxOS Desktop (restore:%lu)"), m_minimizedIds[index]));
                m_titleMarkerPending = true;
            }
            return true;
        });
        pDock->AddItem(pChip);
        m_minimizedSlots.push_back(pChip);
    }

    ui::ButtonVBox* pTrash = MakeTrashTile(this, iconPx);
    pTrash->SetAttribute(DUI_T("margin"), MarginH(iconGap));
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
    PollWindowState();

    if (m_titleMarkerPending) {
        // A tick after the request, so the compositor is certain to have seen
        // it as its own title change.
        m_titleMarkerPending = false;
        SetText(DUI_T("PolluxOS Desktop"));
    }

    GrabPendingThumbnail();

    PollSettings();

    if (m_uiDirty) {
        m_uiDirty = false;
        RebuildUi();
    }

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
        const DString text = life.empty() ? DString(DUI_T("--%"))
                                          : DString(life.c_str()) + DUI_T("%");
        if (m_pBatteryLabel->GetText() != text) {
            m_pBatteryLabel->SetText(text);
        }
    }

    // The menu-bar clock shows minutes: skip SetText when the string did not
    // change, otherwise the timer invalidates the shell every second even
    // though the rendered text is identical.
    if (m_pClockLabel != nullptr && m_pClockLabel->GetText() != DString(menuTimeBuf)) {
        m_pClockLabel->SetText(DString(menuTimeBuf));
    }

    char desktopTimeBuf[32];
    std::snprintf(desktopTimeBuf, sizeof(desktopTimeBuf), "%02d:%02d:%02d",
                  tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    if (m_pDesktopClockLabel != nullptr) {
        m_pDesktopClockLabel->SetText(DString(desktopTimeBuf));
    }

    char desktopDateBuf[80];
    std::snprintf(desktopDateBuf, sizeof(desktopDateBuf), "%04d年%02d月%02d日 %s",
                  tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                  weekdays[tm_now.tm_wday]);
    if (m_pDesktopDateLabel != nullptr) {
        m_pDesktopDateLabel->SetText(DString(desktopDateBuf));
    }
}
