#include "LaunchPadForm.h"
#include "PolluxPaths.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>

namespace {

// macOS Big Sur / Sonoma light palette (matches the PolluxOS shell).
const U8String kMenuPanelLine = "#33000000";
const U8String kTextDark      = "#FF1D1D1F";
const U8String kTextBody      = "#FF3A3A3C";

// Parse one key=value line of a .desktop file into `value` (returns match).
static bool ParseDesktopKey(const std::string& line, const char* key,
                            U8String& value)
{
    const size_t keyLen = std::strlen(key);
    if (line.compare(0, keyLen, key) != 0 || line[keyLen] != '=') {
        return false;
    }
    value = U8String(line.substr(keyLen + 1));
    return true;
}

// Strip freedesktop field codes (%f %F %u %U %d %D %n %N %i %c %k %v %m) that
// /bin/sh cannot interpret.
static void StripExecFieldCodes(U8String& exec)
{
    U8String out;
    out.reserve(exec.size());
    for (size_t i = 0; i < exec.size(); ++i) {
        if (exec[i] == '%' && i + 1 < exec.size()) {
            ++i;   // skip the code char
            continue;
        }
        out.push_back(exec[i]);
    }
    exec = out;
}

static void UseDefaultTerminal(U8String& exec)
{
    if (exec == "foot") {
        exec = "wayst";
    } else if (exec.compare(0, 5, "foot ") == 0) {
        exec.replace(0, 4, "wayst");
    }
}

// PolluxOS ships with a small built-in icon set; the launcher prefers these
// polished SVG icons over system icons/letter glyphs for the apps we know.
static U8String MapKnownPolluxIcon(const U8String& name, const U8String& exec)
{
    const U8String iconNames[] = {
        "utilities-terminal", "x-terminal-emulator", "org.gnome.Terminal",
        "org.kde.konsole", "kitty", "alacritty", "foot",
        "org.gnome.Nautilus", "nautilus", "file-manager",
        "org.xfce.thunar", "thunar",
        "org.gnome.gedit", "gedit", "vim", "org.vim.vim",
        "org.gnome.gvim", "com.visualstudio.code", "code",
        "firefox", "firefox-esr", "org.mozilla.firefox",
        "chromium", "google-chrome", "brave-browser", "epiphany",
        "org.gnome.Settings", "gnome-control-center",
        "preferences-system", "xfce4-settings", "org.kde.systemsettings",
    };
    const U8String polluxdeskIcons[] = {
        "polluxdesk/icons/terminal.svg",
        "polluxdesk/icons/files.svg",
        "polluxdesk/icons/editor.svg",
        "polluxdesk/icons/browser.svg",
        "polluxdesk/icons/settings.svg",
    };
    const int kGroupStart[] = { 0, 7, 12, 19, 26 };
    const int kGroupCount[] = { 7, 5, 7, 7, 5 };

    if (name == "终端" || exec == "wayst" ||
        exec.compare(0, 6, "wayst ") == 0) {
        return "polluxdesk/icons/terminal.svg";
    }
    if (name == "文件" || name == "Files" ||
        name == "Nautilus" || name == "Thunar") {
        return "polluxdesk/icons/files.svg";
    }
    if (name == "编辑器" || name == "Vim" ||
        name == "Editor" || name == "Gedit") {
        return "polluxdesk/icons/editor.svg";
    }
    if (name == "浏览器" || name == "Firefox" ||
        name == "Chromium" || name == "Browser") {
        return "polluxdesk/icons/browser.svg";
    }
    if (name == "设置" || name == "屏幕与外观" ||
        name == "Settings" || name == "System Settings") {
        return "polluxdesk/icons/settings.svg";
    }

    // Search the .desktop Icon= names as well.
    for (int g = 0; g < 5; ++g) {
        for (int i = 0; i < kGroupCount[g]; ++i) {
            if (iconNames[kGroupStart[g] + i] == exec ||
                iconNames[kGroupStart[g] + i] == name ||
                (!exec.empty() && exec.find(iconNames[kGroupStart[g] + i].c_str()) != U8String::npos)) {
                return polluxdeskIcons[g];
            }
        }
    }
    return U8String();
}

static U8String ResolveDesktopIcon(const U8String& name, const U8String& exec,
                                  const U8String& icon)
{
    U8String known = MapKnownPolluxIcon(name, exec);
    if (!known.empty()) {
        return known;
    }

    if (icon.empty()) {
        return "polluxdesk/icons/app-generic.svg";
    }
    if (icon.find('/') != U8String::npos) {
        struct stat st;
        if (stat(icon.c_str(), &st) == 0) {
            return icon;
        }
    }
    const char* home = std::getenv("HOME");
    const U8String roots[] = {
        "/usr/local/share/icons/hicolor", "/usr/share/icons/hicolor",
        "/usr/local/share/pixmaps", "/usr/share/pixmaps",
        home != nullptr ? U8String(home) + "/.local/share/icons/hicolor" : U8String(),
    };
    const char* sizes[] = { "256x256", "128x128", "64x64", "48x48", "32x32", "scalable" };
    const char* exts[] = { ".png", ".svg" };
    for (const U8String& root : roots) {
        if (root.empty()) {
            continue;
        }
        for (const char* size : sizes) {
            for (const char* ext : exts) {
                U8String candidate = root + "/" + size + "/apps/" + icon + ext;
                if (access(candidate.c_str(), R_OK) == 0) {
                    return candidate;
                }
            }
        }
        for (const char* ext : exts) {
            U8String candidate = root + "/" + icon + ext;
            if (access(candidate.c_str(), R_OK) == 0) {
                return candidate;
            }
        }
    }
    return "polluxdesk/icons/app-generic.svg";
}

// First UTF-8 character of `s` (glyph for the app tile icon).
static U8String FirstUtf8Char(const U8String& s)
{
    if (s.empty()) {
        return s;
    }
    const unsigned char c = static_cast<unsigned char>(s[0]);
    size_t len = 1;
    if ((c & 0xE0) == 0xC0) {
        len = 2;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3;
    } else if ((c & 0xF8) == 0xF0) {
        len = 4;
    }
    return s.substr(0, std::min(len, s.size()));
}

} // namespace

LaunchPadForm::LaunchPadForm()
{
}

LaunchPadForm::~LaunchPadForm()
{
}

U8String LaunchPadForm::GetSkinFolder()
{
    return "";
}

U8String LaunchPadForm::GetSkinFile()
{
    // Pure code mode: no layout XML is loaded
    return "";
}

void LaunchPadForm::GetCreateWindowAttributes(ui::WindowCreateAttributes& attrs)
{
    attrs.m_bInitSizeDefined = true;
    attrs.m_szInitSize.cx = 1040;
    attrs.m_szInitSize.cy = 620;
    attrs.m_bShadowAttached = false;
    attrs.m_bShadowAttachedDefined = true;
    attrs.m_bIsLayeredWindow = true;
    attrs.m_bIsLayeredWindowDefined = true;
    // Borderless overlay: the compositor centers and pins it above apps.
    attrs.m_rcCaption = ui::UiRect(0, 0, 0, 0);
    attrs.m_bCaptionDefined = true;
    attrs.m_rcSizeBox = ui::UiRect(0, 0, 0, 0);
    attrs.m_bSizeBoxDefined = true;
    BaseClass::GetCreateWindowAttributes(attrs);
}

void LaunchPadForm::OnInitWindow()
{
    SetShadowAttached(false);
    BuildUi();
    BaseClass::OnInitWindow();

    // Esc dismisses the pad.
    AttachWindowKeyDownMsg([this](const ui::EventArgs& args) {
        if (args.vkCode == ui::kVK_ESCAPE) {
            HidePad();
            return true;
        }
        return false;
    });
    // Losing activation (clicking another app window) also dismisses it.
    AttachWindowKillFocusMsg([this](const ui::EventArgs& /*args*/) {
        if (m_shown) {
            HidePad();
        }
        return true;
    });
}

void LaunchPadForm::ScanDesktopApps(std::vector<DesktopApp>& apps)
{
    const char* home = std::getenv("HOME");
    const U8String dirs[] = {
        "/usr/local/share/applications",
        U8String(home != nullptr ? home : "/home/shxu") + "/.local/share/applications",
        "/usr/share/applications",
    };

    for (const U8String& dir : dirs) {
        DIR* dp = opendir(dir.c_str());
        if (dp == nullptr) {
            continue;
        }
        struct dirent* ent = nullptr;
        while ((ent = readdir(dp)) != nullptr) {
            const char* name = ent->d_name;
            const size_t len = std::strlen(name);
            if (len < 8 || std::strcmp(name + len - 8, ".desktop") != 0) {
                continue;
            }
            std::string path = dir.c_str();
            path += "/";
            path += name;

            FILE* fp = fopen(path.c_str(), "r");
            if (fp == nullptr) {
                continue;
            }
            U8String displayName, exec, icon;
            bool noDisplay = false;
            bool hidden = false;
            bool inMainSection = false;
            char line[1024];
            while (fgets(line, sizeof(line), fp) != nullptr) {
                size_t slen = std::strlen(line);
                while (slen > 0 && (line[slen - 1] == '\n' || line[slen - 1] == '\r')) {
                    line[--slen] = '\0';
                }
                const std::string strLine(line);
                if (strLine.empty() || strLine[0] == '#') {
                    continue;
                }
                if (strLine[0] == '[') {
                    inMainSection = (strLine == "[Desktop Entry]");
                    continue;
                }
                if (!inMainSection) {
                    continue;
                }
                U8String fieldValue;
                if (ParseDesktopKey(strLine, "Name", displayName) ||
                    ParseDesktopKey(strLine, "Exec", exec) ||
                    ParseDesktopKey(strLine, "Icon", icon)) {
                    // keep first (non-localized) value
                } else if (ParseDesktopKey(strLine, "NoDisplay", fieldValue)) {
                    noDisplay = noDisplay || fieldValue == "true";
                } else if (ParseDesktopKey(strLine, "Hidden", fieldValue)) {
                    hidden = hidden || fieldValue == "true";
                }
            }
            fclose(fp);

            if (noDisplay || hidden || displayName.empty() || exec.empty()) {
                continue;
            }
            bool dup = false;
            for (const DesktopApp& app : apps) {
                if (app.name == displayName) {
                    dup = true;
                    break;
                }
            }
            if (dup) {
                continue;
            }
            StripExecFieldCodes(exec);
            UseDefaultTerminal(exec);
            apps.push_back({ displayName, exec, ResolveDesktopIcon(displayName, exec, icon) });
        }
        closedir(dp);
    }

    std::sort(apps.begin(), apps.end(),
              [](const DesktopApp& a, const DesktopApp& b) {
                  return a.name < b.name;
              });

    // PolluxOS system tools are not necessarily installed as desktop files.
    // Keep the native settings application and the activity monitor visible in
    // Apps on a fresh system.
    if (home != nullptr) {
        U8String settingsExec = U8String("\"") + POLLUX_BIN +
            "/polluxdesk_settings\"";
        apps.push_back({ "屏幕与外观", settingsExec, "polluxdesk/icons/settings.svg" });

        U8String activityExec = U8String("\"") + POLLUX_BIN +
            "/polluxdesk_activity\"";
        apps.push_back({ "活动监视器", activityExec, "polluxdesk/icons/activity.svg" });

        U8String wifiExec = U8String("\"") + POLLUX_BIN +
            "/polluxdesk_wifi\"";
        apps.push_back({ "Wi-Fi", wifiExec, "polluxdesk/icons/wifi.svg" });
    }
    std::sort(apps.begin(), apps.end(),
              [](const DesktopApp& a, const DesktopApp& b) {
                  return a.name < b.name;
              });
}

void LaunchPadForm::SetLaunchHandler(
    std::function<void(const U8String& cmd)> handler)
{
    m_launchHandler = std::move(handler);
}

void LaunchPadForm::BuildUi()
{
    // The window IS the frosted panel: transparent root over a rounded
    // frosted card that fills the whole overlay surface.
    ui::VBox* pRoot = new ui::VBox(this);
    pRoot->SetBkColor("#00000000");
    pRoot->SetAttribute("padding", "0,0,0,0");

    ui::VBox* pCard = new ui::VBox(this);
    m_pCard = pCard;
    pCard->SetAttribute("width", "stretch");
    pCard->SetAttribute("height", "stretch");
    pCard->SetAttribute("padding", "16,12,16,16");
    pCard->SetBkColor("#F0F6F7FA");
    // Visible hairline border (macOS Apps / GNOME style framed panel).
    pCard->SetBorderColor("#FF9A9AA0");
    pCard->SetAttribute("border_size", "1");
    pCard->SetStateColorRound(ui::kControlStateNormal, ui::UiSize(18, 18), false);
    pCard->SetAttribute("border_round", "18,18");
    pRoot->AddItem(pCard);

    // Panel caption: centered bold title like GNOME's app grid header.
    ui::Label* pTitle = new ui::Label(this);
    pTitle->SetText("应用");
    pTitle->SetAttribute("font", "system_bold_16");
    pTitle->SetAttribute("text_color", kTextDark);
    pTitle->SetAttribute("text_align", "hcenter,vcenter");
    pTitle->SetAttribute("height", "34");
    pTitle->SetMouseEnabled(false);
    pCard->AddItem(pTitle);

    // Scrollable grid area below the caption: there are usually more apps
    // than fit in the short panel. No child_align here: rows fill the full
    // width so the scroll range is measured correctly.
    m_pGrid = new ui::VScrollBox(this);
    m_pGrid->SetAttribute("height", "stretch");
    m_pGrid->SetAttribute("vscrollbar", "true");
    m_pGrid->SetMouseEnabled("true");
    pCard->AddItem(m_pGrid);

    AttachBox(pRoot);
}

void LaunchPadForm::RefreshAndShow()
{
    ScanDesktopApps(m_apps);
    if (m_apps.empty()) {
        m_apps.push_back({ "终端", "wayst", "polluxdesk/icons/terminal.svg" });
    }

    const int kColumns = 8;
    const size_t totalRows = (m_apps.size() + kColumns - 1) / kColumns;
    m_totalRows = totalRows;
    if (m_firstRow >= static_cast<int>(totalRows)) {
        m_firstRow = 0;
    }

    // Rebuild the grid page inside the scrollable container.
    if (m_pGrid != nullptr) {
        m_pGrid->RemoveAllItems();

        const U8String kTileColors[][2] = {
            { "#FF4C9FDB", "#FF2E6FA3" },
            { "#FF6FBF73", "#FF3E8E50" },
            { "#FF8E7CC3", "#FF5F4B8B" },
            { "#FFF0A35C", "#FFC97A2B" },
            { "#FF5AA9E6", "#FF2F6FAB" },
            { "#FFED6A5E", "#FFB33A30" },
        };
        const int kColorCount =
            static_cast<int>(sizeof(kTileColors) / sizeof(kTileColors[0]));

        const int first = m_firstRow;
        const int last = static_cast<int>(
            std::min(totalRows, static_cast<size_t>(m_firstRow + kMaxVisibleRows)));

        for (int row = first; row < last; ++row) {
            ui::HBox* pRow = new ui::HBox(this);
            pRow->SetAttribute("height", "88");
            pRow->SetAttribute("width", "stretch");
            pRow->SetAttribute("child_align", "hcenter");
            m_pGrid->AddItem(pRow);

            for (int col = 0; col < kColumns; ++col) {
                const size_t i = static_cast<size_t>(row) * kColumns + col;
                if (i >= m_apps.size()) {
                    break;
                }
                const DesktopApp& app = m_apps[i];
                const int colorIndex = static_cast<int>(i) % kColorCount;

                ui::Button* pIcon = new ui::Button(this);
                pIcon->SetText(FirstUtf8Char(app.name));
                pIcon->SetAttribute("font", "system_bold_24");
                pIcon->SetAttribute("text_color", "#FFFFFFFF");
                pIcon->SetAttribute("text_align", "hcenter,vcenter");
                pIcon->SetAttribute("width", "56");
                pIcon->SetAttribute("height", "56");
                pIcon->SetAttribute("margin", "0,0,0,0");
                pIcon->SetBkColor(kTileColors[colorIndex][0]);
                pIcon->SetBkColor2(kTileColors[colorIndex][1]);
                pIcon->SetBkColor2Direction("1");
                if (!app.icon.empty()) {
                    pIcon->SetText("");
                    pIcon->SetBkImage(U8String("file='") + app.icon +
                                      "' width='52' height='52' halign='center' valign='center'");
                }
                pIcon->SetStateColorRound(ui::kControlStateNormal, ui::UiSize(13, 13), false);
                pIcon->SetStateColorRound(ui::kControlStateHot, ui::UiSize(13, 13), false);
                pIcon->SetStateColorRound(ui::kControlStatePushed, ui::UiSize(13, 13), false);
                pIcon->SetAttribute("border_round", "13,13");
                pIcon->SetAttribute("cursor_type", "hand");
                pIcon->SetToolTipText(app.name);

                // The tile is icon + caption; clicking either launches.
                ui::Button* pName = new ui::Button(this);
                pName->SetText(app.name);
                pName->SetAttribute("font", "system_12");
                pName->SetAttribute("text_color", kTextBody);
                pName->SetAttribute("text_align", "hcenter,vcenter");
                pName->SetAttribute("halign", "center");
                pName->SetAttribute("width", "112");
                pName->SetAttribute("height", "22");
                pName->SetStateColor(ui::kControlStateNormal, "#00000000");
                pName->SetStateColorRound(ui::kControlStateNormal, ui::UiSize(6, 6), false);
                pName->SetAttribute("cursor_type", "hand");

                auto onLaunch = [this, i](const ui::EventArgs&) {
                    if (i < m_apps.size()) {
                        // Launch before closing: CloseWnd can end this
                        // client's message loop immediately on Wayland.
                        const U8String command = m_apps[i].exec;
                        if (m_launchHandler) {
                            m_launchHandler(command);
                        } else {
                            LaunchCommand(command);
                        }
                    }
                    HidePad();
                    return true;
                };
                pIcon->AttachClick(onLaunch);
                pName->AttachClick(onLaunch);

                ui::VBox* pTile = new ui::VBox(this);
                pTile->SetAttribute("width", "112");
                pTile->SetAttribute("child_align", "hcenter,vcenter");
                ui::HBox* pIconCell = new ui::HBox(this);
                pIconCell->SetAttribute("width", "stretch");
                pIconCell->SetAttribute("height", "56");
                pIconCell->SetAttribute("child_align", "hcenter,vcenter");
                pIconCell->AddItem(pIcon);
                pTile->AddItem(pIconCell);
                pTile->AddItem(pName);
                pRow->AddItem(pTile);
            }
        }
    }

    // Window size: caption + visible rows (+ pager row when needed).
    const size_t visibleRows = std::min(totalRows, static_cast<size_t>(6));
    const bool hasPager = totalRows > static_cast<size_t>(kMaxVisibleRows);
    const int width = kColumns * 112 + 2 * 32;
    (void)visibleRows;
    int height = 620;
    (void)hasPager;
    SetWindowPos(ui::InsertAfterWnd(), 0, 0, width, height, 0);

    // Pager row (prev / page indicator / next), fixed below the scroll area
    // so paging works even without a mouse wheel.
    if (m_pCard != nullptr) {
        if (m_pNav != nullptr) {
            m_pCard->RemoveItem(m_pNav);
            m_pNav = nullptr;
        }
        if (hasPager) {
            const U8String kPageText = ui::StringUtil::Printf(
                "%d / %d", m_firstRow + 1,
                static_cast<int>((totalRows - 1) / kMaxVisibleRows) + 1);

            m_pNav = new ui::HBox(this);
            m_pNav->SetAttribute("height", "32");
            m_pNav->SetAttribute("child_align", "hcenter");
            m_pCard->AddItem(m_pNav);

            auto makeNavButton = [this](const U8String& text) {
                ui::Button* pBtn = new ui::Button(this);
                pBtn->SetText(text);
                pBtn->SetAttribute("font", "system_bold_14");
                pBtn->SetAttribute("text_color", kTextBody);
                pBtn->SetAttribute("text_align", "hcenter,vcenter");
                pBtn->SetAttribute("width", "64");
                pBtn->SetAttribute("height", "28");
                pBtn->SetAttribute("margin", "8,0,8,0");
                pBtn->SetStateColor(ui::kControlStateNormal, "#00000000");
                pBtn->SetStateColor(ui::kControlStateHot, "#330A84FF");
                pBtn->SetStateColorRound(ui::kControlStateNormal, ui::UiSize(6, 6), false);
                pBtn->SetStateColorRound(ui::kControlStateHot, ui::UiSize(6, 6), false);
                pBtn->SetAttribute("cursor_type", "hand");
                return pBtn;
            };

            ui::Button* pPrev = makeNavButton("‹ 上一页");
            pPrev->AttachClick([this](const ui::EventArgs&) {
                if (m_firstRow > 0) {
                    m_firstRow -= kMaxVisibleRows;
                    RefreshAndShow();
                }
                return true;
            });
            m_pNav->AddItem(pPrev);

            ui::Label* pIndicator = new ui::Label(this);
            pIndicator->SetText(kPageText);
            pIndicator->SetAttribute("font", "system_12");
            pIndicator->SetAttribute("text_color", kTextDark);
            pIndicator->SetAttribute("text_align", "hcenter,vcenter");
            pIndicator->SetAttribute("width", "80");
            pIndicator->SetAttribute("height", "28");
            pIndicator->SetMouseEnabled(false);
            m_pNav->AddItem(pIndicator);

            ui::Button* pNext = makeNavButton("下一页 ›");
            pNext->AttachClick([this](const ui::EventArgs&) {
                if (static_cast<size_t>(m_firstRow + kMaxVisibleRows) < m_totalRows) {
                    m_firstRow += kMaxVisibleRows;
                    RefreshAndShow();
                }
                return true;
            });
            m_pNav->AddItem(pNext);
        }
    }

    m_shown = true;
    ShowWindow(ui::kSW_SHOW_NORMAL);
}

void LaunchPadForm::LaunchCommand(const U8String& command)
{
    if (command.empty()) {
        return;
    }
    printf("[polluxdesk-apps] launch: %s\n", command.c_str());
    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        perror("[polluxdesk-apps] exec");
        _exit(127);
    }
}

void LaunchPadForm::HidePad()
{
    // Destroy the overlay window (ShowWindow(HIDE) does not reliably unmap
    // on the Wayland backend). The shell recreates it on next open; the
    // weak-flag guard in PolluxOSForm tracks our lifetime.
    if (m_closing) {
        return;
    }
    m_closing = true;
    m_shown = false;
    CloseWnd(ui::kWindowCloseNormal);
}
