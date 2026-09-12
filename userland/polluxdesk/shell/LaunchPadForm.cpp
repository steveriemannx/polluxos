#include "LaunchPadForm.h"
#include "PolluxPaths.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>

namespace {

// macOS Big Sur / Sonoma light palette (matches the PolluxOS shell).
const DString kMenuPanelLine = DUI_T("#33000000");
const DString kTextDark      = DUI_T("#FF1D1D1F");
const DString kTextBody      = DUI_T("#FF3A3A3C");

// Parse one key=value line of a .desktop file into `value` (returns match).
static bool ParseDesktopKey(const std::string& line, const char* key,
                            DString& value)
{
    const size_t keyLen = std::strlen(key);
    if (line.compare(0, keyLen, key) != 0 || line[keyLen] != '=') {
        return false;
    }
    value = DString(line.substr(keyLen + 1));
    return true;
}

// Strip freedesktop field codes (%f %F %u %U %d %D %n %N %i %c %k %v %m) that
// /bin/sh cannot interpret.
static void StripExecFieldCodes(DString& exec)
{
    DString out;
    out.reserve(exec.size());
    for (size_t i = 0; i < exec.size(); ++i) {
        if (exec[i] == DUI_T('%') && i + 1 < exec.size()) {
            ++i;   // skip the code char
            continue;
        }
        out.push_back(exec[i]);
    }
    exec = out;
}

static void UseDefaultTerminal(DString& exec)
{
    if (exec == DUI_T("foot")) {
        exec = DUI_T("wayst");
    } else if (exec.compare(0, 5, DUI_T("foot ")) == 0) {
        exec.replace(0, 4, DUI_T("wayst"));
    }
}

// PolluxOS ships with a small built-in icon set; the launcher prefers these
// polished SVG icons over system icons/letter glyphs for the apps we know.
static DString MapKnownPolluxIcon(const DString& name, const DString& exec)
{
    const DString iconNames[] = {
        DUI_T("utilities-terminal"), DUI_T("x-terminal-emulator"), DUI_T("org.gnome.Terminal"),
        DUI_T("org.kde.konsole"), DUI_T("kitty"), DUI_T("alacritty"), DUI_T("foot"),
        DUI_T("org.gnome.Nautilus"), DUI_T("nautilus"), DUI_T("file-manager"),
        DUI_T("org.xfce.thunar"), DUI_T("thunar"),
        DUI_T("org.gnome.gedit"), DUI_T("gedit"), DUI_T("vim"), DUI_T("org.vim.vim"),
        DUI_T("org.gnome.gvim"), DUI_T("com.visualstudio.code"), DUI_T("code"),
        DUI_T("firefox"), DUI_T("firefox-esr"), DUI_T("org.mozilla.firefox"),
        DUI_T("chromium"), DUI_T("google-chrome"), DUI_T("brave-browser"), DUI_T("epiphany"),
        DUI_T("org.gnome.Settings"), DUI_T("gnome-control-center"),
        DUI_T("preferences-system"), DUI_T("xfce4-settings"), DUI_T("org.kde.systemsettings"),
    };
    const DString polluxdeskIcons[] = {
        DUI_T("polluxdesk/icons/terminal.svg"),
        DUI_T("polluxdesk/icons/files.svg"),
        DUI_T("polluxdesk/icons/editor.svg"),
        DUI_T("polluxdesk/icons/browser.svg"),
        DUI_T("polluxdesk/icons/settings.svg"),
    };
    const int kGroupStart[] = { 0, 7, 12, 19, 26 };
    const int kGroupCount[] = { 7, 5, 7, 7, 5 };

    if (name == DUI_T("终端") || exec == DUI_T("wayst") ||
        exec.compare(0, 6, DUI_T("wayst ")) == 0) {
        return DUI_T("polluxdesk/icons/terminal.svg");
    }
    if (name == DUI_T("文件") || name == DUI_T("Files") ||
        name == DUI_T("Nautilus") || name == DUI_T("Thunar")) {
        return DUI_T("polluxdesk/icons/files.svg");
    }
    if (name == DUI_T("编辑器") || name == DUI_T("Vim") ||
        name == DUI_T("Editor") || name == DUI_T("Gedit")) {
        return DUI_T("polluxdesk/icons/editor.svg");
    }
    if (name == DUI_T("浏览器") || name == DUI_T("Firefox") ||
        name == DUI_T("Chromium") || name == DUI_T("Browser")) {
        return DUI_T("polluxdesk/icons/browser.svg");
    }
    if (name == DUI_T("设置") || name == DUI_T("屏幕与外观") ||
        name == DUI_T("Settings") || name == DUI_T("System Settings")) {
        return DUI_T("polluxdesk/icons/settings.svg");
    }

    // Search the .desktop Icon= names as well.
    for (int g = 0; g < 5; ++g) {
        for (int i = 0; i < kGroupCount[g]; ++i) {
            if (iconNames[kGroupStart[g] + i] == exec ||
                iconNames[kGroupStart[g] + i] == name ||
                (!exec.empty() && exec.find(iconNames[kGroupStart[g] + i].c_str()) != DString::npos)) {
                return polluxdeskIcons[g];
            }
        }
    }
    return DString();
}

static DString ResolveDesktopIcon(const DString& name, const DString& exec,
                                  const DString& icon)
{
    DString known = MapKnownPolluxIcon(name, exec);
    if (!known.empty()) {
        return known;
    }

    if (icon.empty()) {
        return DUI_T("polluxdesk/icons/app-generic.svg");
    }
    if (icon.find(DUI_T('/')) != DString::npos) {
        struct stat st;
        if (stat(icon.c_str(), &st) == 0) {
            return icon;
        }
    }

    // Try the usual hicolor/pixmaps locations before falling back to the
    // bundled generic icon.
    const char* home = std::getenv("HOME");
    const DString roots[] = {
        DUI_T("/usr/local/share/icons/hicolor"), DUI_T("/usr/share/icons/hicolor"),
        DUI_T("/usr/local/share/pixmaps"), DUI_T("/usr/share/pixmaps"),
        home != nullptr ? DString(home) + DUI_T("/.local/share/icons/hicolor") : DString(),
    };
    const char* sizes[] = { "256x256", "128x128", "64x64", "48x48", "32x32", "scalable" };
    const char* exts[] = { ".png", ".svg" };
    for (const DString& root : roots) {
        if (root.empty()) {
            continue;
        }
        for (const char* size : sizes) {
            for (const char* ext : exts) {
                DString candidate = root + DUI_T("/") + size + DUI_T("/apps/") + icon + ext;
                if (access(candidate.c_str(), R_OK) == 0) {
                    return candidate;
                }
            }
        }
        for (const char* ext : exts) {
            DString candidate = root + DUI_T("/") + icon + ext;
            if (access(candidate.c_str(), R_OK) == 0) {
                return candidate;
            }
        }
    }
    return DUI_T("polluxdesk/icons/app-generic.svg");
}

// First UTF-8 character of `s` (glyph for the app tile icon).
static DString FirstUtf8Char(const DString& s)
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

DString LaunchPadForm::GetSkinFolder()
{
    return DUI_T("");
}

DString LaunchPadForm::GetSkinFile()
{
    // Pure code mode: no layout XML is loaded
    return DUI_T("");
}

void LaunchPadForm::GetCreateWindowAttributes(ui::WindowCreateAttributes& attrs)
{
    attrs.m_bInitSizeDefined = true;
    attrs.m_szInitSize.cx = 1040;
    attrs.m_szInitSize.cy = 320;
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
    const DString dirs[] = {
        DUI_T("/usr/local/share/applications"),
        DString(home != nullptr ? home : "/home/shxu") + DUI_T("/.local/share/applications"),
        DUI_T("/usr/share/applications"),
    };

    for (const DString& dir : dirs) {
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
            DString displayName, exec, icon;
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
                DString fieldValue;
                if (ParseDesktopKey(strLine, "Name", displayName) ||
                    ParseDesktopKey(strLine, "Exec", exec) ||
                    ParseDesktopKey(strLine, "Icon", icon)) {
                    // keep first (non-localized) value
                } else if (ParseDesktopKey(strLine, "NoDisplay", fieldValue)) {
                    noDisplay = noDisplay || fieldValue == DUI_T("true");
                } else if (ParseDesktopKey(strLine, "Hidden", fieldValue)) {
                    hidden = hidden || fieldValue == DUI_T("true");
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
    // the launcher on a fresh system.
    if (home != nullptr) {
        DString settingsExec = DString("\"") + home +
            DUI_T("\"" POLLUX_BIN "/polluxdesk_settings\"");
        apps.push_back({ DUI_T("屏幕与外观"), settingsExec,
                         DUI_T("polluxdesk/icons/settings.svg") });

        DString activityExec = DString("\"") + home +
            DUI_T("\"" POLLUX_BIN "/polluxdesk_activity\"");
        apps.push_back({ DUI_T("活动监视器"), activityExec,
                         DUI_T("polluxdesk/icons/activity.svg") });
    }
    std::sort(apps.begin(), apps.end(),
              [](const DesktopApp& a, const DesktopApp& b) {
                  return a.name < b.name;
              });
}

void LaunchPadForm::SetLaunchHandler(
    std::function<void(const DString& cmd)> handler)
{
    m_launchHandler = std::move(handler);
}

void LaunchPadForm::BuildUi()
{
    // The window IS the frosted panel: transparent root over a rounded
    // frosted card that fills the whole overlay surface.
    ui::VBox* pRoot = new ui::VBox(this);
    pRoot->SetBkColor(DUI_T("#00000000"));
    pRoot->SetAttribute(DUI_T("padding"), DUI_T("0,0,0,0"));

    ui::VBox* pCard = new ui::VBox(this);
    m_pCard = pCard;
    pCard->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    pCard->SetAttribute(DUI_T("height"), DUI_T("stretch"));
    pCard->SetAttribute(DUI_T("padding"), DUI_T("16,12,16,16"));
    pCard->SetBkColor(DUI_T("#F0F6F7FA"));
    // Visible hairline border (macOS Apps / GNOME style framed panel).
    pCard->SetBorderColor(DUI_T("#FF9A9AA0"));
    pCard->SetAttribute(DUI_T("border_size"), DUI_T("1"));
    pCard->SetStateColorRound(ui::kControlStateNormal, ui::UiSize(18, 18), false);
    pCard->SetAttribute(DUI_T("border_round"), DUI_T("18,18"));
    pRoot->AddItem(pCard);

    // Panel caption: centered bold title like GNOME's app grid header.
    ui::Label* pTitle = new ui::Label(this);
    pTitle->SetText(DUI_T("应用"));
    pTitle->SetAttribute(DUI_T("font"), DUI_T("system_bold_16"));
    pTitle->SetAttribute(DUI_T("text_color"), kTextDark);
    pTitle->SetAttribute(DUI_T("text_align"), DUI_T("hcenter,vcenter"));
    pTitle->SetAttribute(DUI_T("height"), DUI_T("34"));
    pTitle->SetMouseEnabled(false);
    pCard->AddItem(pTitle);

    // Scrollable grid area below the caption: there are usually more apps
    // than fit in the short panel. No child_align here: rows fill the full
    // width so the scroll range is measured correctly.
    m_pGrid = new ui::VScrollBox(this);
    m_pGrid->SetAttribute(DUI_T("height"), DUI_T("stretch"));
    m_pGrid->SetAttribute(DUI_T("vscrollbar"), DUI_T("true"));
    m_pGrid->SetMouseEnabled(DUI_T("true"));
    pCard->AddItem(m_pGrid);

    AttachBox(pRoot);
}

void LaunchPadForm::RefreshAndShow()
{
    ScanDesktopApps(m_apps);
    if (m_apps.empty()) {
        m_apps.push_back({ DUI_T("终端"), DUI_T("wayst"), DUI_T("polluxdesk/icons/terminal.svg") });
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

        const DString kTileColors[][2] = {
            { DUI_T("#FF4C9FDB"), DUI_T("#FF2E6FA3") },
            { DUI_T("#FF6FBF73"), DUI_T("#FF3E8E50") },
            { DUI_T("#FF8E7CC3"), DUI_T("#FF5F4B8B") },
            { DUI_T("#FFF0A35C"), DUI_T("#FFC97A2B") },
            { DUI_T("#FF5AA9E6"), DUI_T("#FF2F6FAB") },
            { DUI_T("#FFED6A5E"), DUI_T("#FFB33A30") },
        };
        const int kColorCount =
            static_cast<int>(sizeof(kTileColors) / sizeof(kTileColors[0]));

        const int first = m_firstRow;
        const int last = static_cast<int>(
            std::min(totalRows, static_cast<size_t>(m_firstRow + kMaxVisibleRows)));

        for (int row = first; row < last; ++row) {
            ui::HBox* pRow = new ui::HBox(this);
            pRow->SetAttribute(DUI_T("height"), DUI_T("88"));
            pRow->SetAttribute(DUI_T("width"), DUI_T("stretch"));
            pRow->SetAttribute(DUI_T("child_align"), DUI_T("hcenter"));
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
                pIcon->SetAttribute(DUI_T("font"), DUI_T("system_bold_24"));
                pIcon->SetAttribute(DUI_T("text_color"), DUI_T("#FFFFFFFF"));
                pIcon->SetAttribute(DUI_T("text_align"), DUI_T("hcenter,vcenter"));
                pIcon->SetAttribute(DUI_T("width"), DUI_T("56"));
                pIcon->SetAttribute(DUI_T("height"), DUI_T("56"));
                pIcon->SetAttribute(DUI_T("margin"), DUI_T("14,0,14,0"));
                pIcon->SetBkColor(kTileColors[colorIndex][0]);
                pIcon->SetBkColor2(kTileColors[colorIndex][1]);
                pIcon->SetBkColor2Direction(DUI_T("1"));
                if (!app.icon.empty()) {
                    pIcon->SetText(DUI_T(""));
                    pIcon->SetBkImage(DString(DUI_T("file='")) + app.icon +
                                      DUI_T("' width='52' height='52' halign='center' valign='center'"));
                }
                pIcon->SetStateColorRound(ui::kControlStateNormal, ui::UiSize(13, 13), false);
                pIcon->SetStateColorRound(ui::kControlStateHot, ui::UiSize(13, 13), false);
                pIcon->SetStateColorRound(ui::kControlStatePushed, ui::UiSize(13, 13), false);
                pIcon->SetAttribute(DUI_T("border_round"), DUI_T("13,13"));
                pIcon->SetAttribute(DUI_T("cursor_type"), DUI_T("hand"));
                pIcon->SetToolTipText(app.name);

                // The tile is icon + caption; clicking either launches.
                ui::Button* pName = new ui::Button(this);
                pName->SetText(app.name);
                pName->SetAttribute(DUI_T("font"), DUI_T("system_12"));
                pName->SetAttribute(DUI_T("text_color"), kTextBody);
                pName->SetAttribute(DUI_T("text_align"), DUI_T("hcenter,vcenter"));
                pName->SetAttribute(DUI_T("width"), DUI_T("112"));
                pName->SetAttribute(DUI_T("height"), DUI_T("22"));
                pName->SetStateColor(ui::kControlStateNormal, DUI_T("#00000000"));
                pName->SetStateColorRound(ui::kControlStateNormal, ui::UiSize(6, 6), false);
                pName->SetAttribute(DUI_T("cursor_type"), DUI_T("hand"));

                auto onLaunch = [this, i](const ui::EventArgs&) {
                    HidePad();
                    if (m_launchHandler && i < m_apps.size()) {
                        m_launchHandler(m_apps[i].exec);
                    }
                    return true;
                };
                pIcon->AttachClick(onLaunch);
                pName->AttachClick(onLaunch);

            ui::VBox* pTile = new ui::VBox(this);
            pTile->SetAttribute(DUI_T("width"), DUI_T("112"));
            pTile->AddItem(pIcon);
                pTile->AddItem(pName);
                pRow->AddItem(pTile);
            }
        }
    }

    // Window size: caption + visible rows (+ pager row when needed).
    const size_t visibleRows =
        std::min(totalRows, static_cast<size_t>(kMaxVisibleRows));
    const bool hasPager = totalRows > static_cast<size_t>(kMaxVisibleRows);
    const int width = kColumns * 112 + 2 * 32;
    int height = static_cast<int>(visibleRows) * 88 + 34 + 2 * 16;
    if (hasPager) {
        height += 36;
    }
    SetWindowPos(ui::InsertAfterWnd(), 0, 0, width, height, 0);

    // Pager row (prev / page indicator / next), fixed below the scroll area
    // so paging works even without a mouse wheel.
    if (m_pCard != nullptr) {
        if (m_pNav != nullptr) {
            m_pCard->RemoveItem(m_pNav);
            m_pNav = nullptr;
        }
        if (hasPager) {
            const DString kPageText = ui::StringUtil::Printf(
                DUI_T("%d / %d"), m_firstRow + 1,
                static_cast<int>((totalRows - 1) / kMaxVisibleRows) + 1);

            m_pNav = new ui::HBox(this);
            m_pNav->SetAttribute(DUI_T("height"), DUI_T("32"));
            m_pNav->SetAttribute(DUI_T("child_align"), DUI_T("hcenter"));
            m_pCard->AddItem(m_pNav);

            auto makeNavButton = [this](const DString& text) {
                ui::Button* pBtn = new ui::Button(this);
                pBtn->SetText(text);
                pBtn->SetAttribute(DUI_T("font"), DUI_T("system_bold_14"));
                pBtn->SetAttribute(DUI_T("text_color"), kTextBody);
                pBtn->SetAttribute(DUI_T("text_align"), DUI_T("hcenter,vcenter"));
                pBtn->SetAttribute(DUI_T("width"), DUI_T("64"));
                pBtn->SetAttribute(DUI_T("height"), DUI_T("28"));
                pBtn->SetAttribute(DUI_T("margin"), DUI_T("8,0,8,0"));
                pBtn->SetStateColor(ui::kControlStateNormal, DUI_T("#00000000"));
                pBtn->SetStateColor(ui::kControlStateHot, DUI_T("#330A84FF"));
                pBtn->SetStateColorRound(ui::kControlStateNormal, ui::UiSize(6, 6), false);
                pBtn->SetStateColorRound(ui::kControlStateHot, ui::UiSize(6, 6), false);
                pBtn->SetAttribute(DUI_T("cursor_type"), DUI_T("hand"));
                return pBtn;
            };

            ui::Button* pPrev = makeNavButton(DUI_T("‹ 上一页"));
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
            pIndicator->SetAttribute(DUI_T("font"), DUI_T("system_12"));
            pIndicator->SetAttribute(DUI_T("text_color"), kTextDark);
            pIndicator->SetAttribute(DUI_T("text_align"), DUI_T("hcenter,vcenter"));
            pIndicator->SetAttribute(DUI_T("width"), DUI_T("80"));
            pIndicator->SetAttribute(DUI_T("height"), DUI_T("28"));
            pIndicator->SetMouseEnabled(false);
            m_pNav->AddItem(pIndicator);

            ui::Button* pNext = makeNavButton(DUI_T("下一页 ›"));
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
