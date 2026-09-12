#include "SettingsForm.h"

#include "PolluxPaths.h"
#include "PolluxSettings.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>

namespace {

const DString kBg = DUI_T("#F2FFFFFF");
const DString kCard = DUI_T("#F8FFFFFF");
const DString kBorder = DUI_T("#559A9AA0");
const DString kText = DUI_T("#FF1D1D1F");
const DString kHint = DUI_T("#FF6E6E73");
const DString kAccent = DUI_T("#FF0A84FF");

void StyleButton(ui::Button* button)
{
    button->SetAttribute(DUI_T("font"), DUI_T("system_14"));
    button->SetAttribute(DUI_T("text_color"), kText);
    button->SetAttribute(DUI_T("height"), DUI_T("38"));
    button->SetAttribute(DUI_T("width"), DUI_T("190"));
    button->SetAttribute(DUI_T("margin"), DUI_T("4,4,4,4"));
    button->SetStateColor(ui::kControlStateNormal, DUI_T("#FFFFFFFF"));
    button->SetStateColor(ui::kControlStateHot, DUI_T("#220A84FF"));
    button->SetStateColorRound(ui::kControlStateNormal, ui::UiSize(9, 9), false);
    button->SetStateColorRound(ui::kControlStateHot, ui::UiSize(9, 9), false);
    button->SetAttribute(DUI_T("border_round"), DUI_T("9,9"));
    button->SetAttribute(DUI_T("cursor_type"), DUI_T("hand"));
}

void StyleSectionButton(ui::Button* button)
{
    button->SetAttribute(DUI_T("font"), DUI_T("system_14"));
    button->SetAttribute(DUI_T("text_color"), kText);
    button->SetAttribute(DUI_T("height"), DUI_T("42"));
    button->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    button->SetAttribute(DUI_T("margin"), DUI_T("8,2,8,2"));
    button->SetAttribute(DUI_T("text_padding"), DUI_T("14,0,0,0"));
    button->SetStateColor(ui::kControlStateNormal, DUI_T("#00000000"));
    button->SetStateColor(ui::kControlStateHot, DUI_T("#220A84FF"));
    button->SetStateColorRound(ui::kControlStateNormal, ui::UiSize(8, 8), false);
    button->SetStateColorRound(ui::kControlStateHot, ui::UiSize(8, 8), false);
    button->SetAttribute(DUI_T("cursor_type"), DUI_T("hand"));
}

} // namespace

SettingsForm::SettingsForm() = default;
SettingsForm::~SettingsForm() = default;

DString SettingsForm::GetSkinFolder() { return DUI_T(""); }
DString SettingsForm::GetSkinFile() { return DUI_T(""); }

void SettingsForm::GetCreateWindowAttributes(ui::WindowCreateAttributes& attrs)
{
    attrs.m_bInitSizeDefined = true;
    attrs.m_szInitSize.cx = 860;
    attrs.m_szInitSize.cy = 600;
    attrs.m_bShadowAttached = false;
    attrs.m_bShadowAttachedDefined = true;
    attrs.m_bIsLayeredWindow = true;
    attrs.m_bIsLayeredWindowDefined = true;
    attrs.m_rcCaption = ui::UiRect(0, 0, 0, 0);
    attrs.m_bCaptionDefined = true;
    attrs.m_rcSizeBox = ui::UiRect(0, 0, 0, 0);
    attrs.m_bSizeBoxDefined = true;
    BaseClass::GetCreateWindowAttributes(attrs);
}

void SettingsForm::OnInitWindow()
{
    SetShadowAttached(false);
    BuildUi();
    BaseClass::OnInitWindow();
}

void SettingsForm::BuildUi()
{
    ui::VBox* root = new ui::VBox(this);
    root->SetBkColor(kBg);
    root->SetAttribute(DUI_T("padding"), DUI_T("22,22,22,22"));

    ui::Label* title = new ui::Label(this);
    title->SetText(DUI_T("系统设置"));
    title->SetAttribute(DUI_T("font"), DUI_T("system_bold_24"));
    title->SetAttribute(DUI_T("text_color"), kText);
    title->SetAttribute(DUI_T("height"), DUI_T("42"));
    root->AddItem(title);

    ui::HBox* content = new ui::HBox(this);
    content->SetAttribute(DUI_T("height"), DUI_T("stretch"));
    root->AddItem(content);

    // Left sidebar, macOS System Settings style.
    ui::VBox* sidebar = new ui::VBox(this);
    sidebar->SetAttribute(DUI_T("width"), DUI_T("210"));
    sidebar->SetBkColor(kCard);
    sidebar->SetBorderColor(kBorder);
    sidebar->SetAttribute(DUI_T("border_size"), DUI_T("1"));
    sidebar->SetAttribute(DUI_T("border_round"), DUI_T("12,12"));
    sidebar->SetStateColorRound(ui::kControlStateNormal, ui::UiSize(12, 12), false);
    content->AddItem(sidebar);

    const struct { const char* label; int index; } sections[] = {
        { "壁纸", 0 }, { "网络", 1 }, { "Wi-Fi", 2 }, { "关于", 3 },
    };
    for (const auto& section : sections) {
        ui::Button* sectionButton = new ui::Button(this);
        sectionButton->SetText(DString(section.label));
        StyleSectionButton(sectionButton);
        sectionButton->AttachClick([this, section](const ui::EventArgs&) {
            ShowSection(section.index);
            return true;
        });
        sidebar->AddItem(sectionButton);
    }

    // Right content panel.
    m_pPanel = new ui::VBox(this);
    m_pPanel->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    m_pPanel->SetAttribute(DUI_T("padding"), DUI_T("18,0,0,18"));
    content->AddItem(m_pPanel);

    AttachBox(root);
    ShowSection(0);
}

void SettingsForm::ShowSection(int index)
{
    m_currentSection = index;
    m_pStatus = nullptr;
    if (m_pPanel == nullptr) {
        return;
    }
    m_pPanel->RemoveAllItems();

    switch (index) {
    case 1:
        BuildNetworkPanel(m_pPanel);
        break;
    case 2:
        BuildWifiPanel(m_pPanel);
        break;
    case 3:
        BuildAboutPanel(m_pPanel);
        break;
    case 0:
    default:
        BuildWallpaperPanel(m_pPanel);
        break;
    }
}

void SettingsForm::BuildWallpaperPanel(ui::VBox* panel)
{
    ui::Label* wallpaper = new ui::Label(this);
    wallpaper->SetText(DUI_T("壁纸"));
    wallpaper->SetAttribute(DUI_T("font"), DUI_T("system_bold_18"));
    wallpaper->SetAttribute(DUI_T("text_color"), kText);
    wallpaper->SetAttribute(DUI_T("height"), DUI_T("34"));
    panel->AddItem(wallpaper);

    struct Wallpaper { const char* label; const char* value; } wallpapers[] = {
        { "海蓝", "blue" }, { "紫霞", "purple" },
        { "深夜", "dark" }, { "森林", "green" },
    };
    for (const Wallpaper& item : wallpapers) {
        ui::Button* button = new ui::Button(this);
        button->SetText(DString(item.label));
        StyleButton(button);
        button->AttachClick([this, item](const ui::EventArgs&) {
            ApplyWallpaper(item.value);
            return true;
        });
        panel->AddItem(button);
    }

    ui::Label* resolution = new ui::Label(this);
    resolution->SetText(DUI_T("分辨率"));
    resolution->SetAttribute(DUI_T("font"), DUI_T("system_bold_18"));
    resolution->SetAttribute(DUI_T("text_color"), kText);
    resolution->SetAttribute(DUI_T("height"), DUI_T("34"));
    resolution->SetAttribute(DUI_T("margin"), DUI_T("16,0,0,0"));
    panel->AddItem(resolution);

    const char* resolutions[] = { "1920x1080", "1600x900", "1280x720" };
    for (const char* value : resolutions) {
        ui::Button* button = new ui::Button(this);
        button->SetText(DString(value));
        StyleButton(button);
        button->AttachClick([this, value](const ui::EventArgs&) {
            ApplyResolution(value);
            return true;
        });
        panel->AddItem(button);
    }

    m_pStatus = new ui::Label(this);
    m_pStatus->SetText(DUI_T("修改会实时应用到桌面"));
    m_pStatus->SetAttribute(DUI_T("font"), DUI_T("system_12"));
    m_pStatus->SetAttribute(DUI_T("text_color"), kHint);
    m_pStatus->SetAttribute(DUI_T("height"), DUI_T("30"));
    panel->AddItem(m_pStatus);
}

void SettingsForm::BuildNetworkPanel(ui::VBox* panel)
{
    ui::Label* title = new ui::Label(this);
    title->SetText(DUI_T("网络"));
    title->SetAttribute(DUI_T("font"), DUI_T("system_bold_18"));
    title->SetAttribute(DUI_T("text_color"), kText);
    title->SetAttribute(DUI_T("height"), DUI_T("34"));
    panel->AddItem(title);

    ui::Label* ethernet = new ui::Label(this);
    ethernet->SetText(DUI_T("以太网  已连接"));
    ethernet->SetAttribute(DUI_T("font"), DUI_T("system_16"));
    ethernet->SetAttribute(DUI_T("text_color"), kText);
    ethernet->SetAttribute(DUI_T("height"), DUI_T("32"));
    panel->AddItem(ethernet);

    ui::Label* ip = new ui::Label(this);
    ip->SetText(DUI_T("IP 地址 192.168.0.104"));
    ip->SetAttribute(DUI_T("font"), DUI_T("system_14"));
    ip->SetAttribute(DUI_T("text_color"), kHint);
    ip->SetAttribute(DUI_T("height"), DUI_T("28"));
    panel->AddItem(ip);

    ui::Label* mask = new ui::Label(this);
    mask->SetText(DUI_T("子网掩码 255.255.255.0"));
    mask->SetAttribute(DUI_T("font"), DUI_T("system_14"));
    mask->SetAttribute(DUI_T("text_color"), kHint);
    mask->SetAttribute(DUI_T("height"), DUI_T("28"));
    panel->AddItem(mask);

    ui::Button* refresh = new ui::Button(this);
    refresh->SetText(DUI_T("刷新网络状态"));
    StyleButton(refresh);
    refresh->AttachClick([this](const ui::EventArgs&) {
        NotifyCompositor();
        if (m_pStatus != nullptr) {
            m_pStatus->SetText(DUI_T("已通知 compositor 刷新状态"));
        }
        return true;
    });
    panel->AddItem(refresh);

    m_pStatus = new ui::Label(this);
    m_pStatus->SetText(DUI_T("网络状态由系统自动管理"));
    m_pStatus->SetAttribute(DUI_T("font"), DUI_T("system_12"));
    m_pStatus->SetAttribute(DUI_T("text_color"), kHint);
    m_pStatus->SetAttribute(DUI_T("height"), DUI_T("30"));
    panel->AddItem(m_pStatus);
}

void SettingsForm::BuildWifiPanel(ui::VBox* panel)
{
    ui::Label* title = new ui::Label(this);
    title->SetText(DUI_T("Wi-Fi"));
    title->SetAttribute(DUI_T("font"), DUI_T("system_bold_18"));
    title->SetAttribute(DUI_T("text_color"), kText);
    title->SetAttribute(DUI_T("height"), DUI_T("34"));
    panel->AddItem(title);

    // The real state of the interface, not a switch that pretends.
    std::string ssid, address;
    const std::string interfaces = pollux::Run("ifconfig -l");
    size_t at = 0;
    while (at < interfaces.size()) {
        size_t end = interfaces.find_first_of(" \t\n", at);
        if (end == std::string::npos) {
            end = interfaces.size();
        }
        const std::string name = interfaces.substr(at, end - at);
        if (name.compare(0, 4, "wlan") == 0) {
            const std::string config = pollux::Run(("ifconfig " + name).c_str());
            const size_t ssidAt = config.find("ssid ");
            const size_t ssidEnd = config.find(" channel ", ssidAt);
            if (ssidAt != std::string::npos && ssidEnd != std::string::npos) {
                ssid = config.substr(ssidAt + 5, ssidEnd - ssidAt - 5);
            }
            const size_t inetAt = config.find("inet ");
            if (inetAt != std::string::npos) {
                const size_t ipEnd = config.find_first_of(" \t\n", inetAt + 5);
                address = config.substr(inetAt + 5, ipEnd - inetAt - 5);
            }
            break;
        }
        at = end + 1;
    }

    ui::Label* status = new ui::Label(this);
    status->SetText(ssid.empty() ? DUI_T("未连接") : DString(ssid.c_str()));
    status->SetAttribute(DUI_T("font"), DUI_T("system_bold_16"));
    status->SetAttribute(DUI_T("text_color"), kText);
    status->SetAttribute(DUI_T("height"), DUI_T("30"));
    panel->AddItem(status);

    ui::Label* detail = new ui::Label(this);
    detail->SetText(address.empty() ? DUI_T("没有分配地址") : DString(address.c_str()));
    detail->SetAttribute(DUI_T("font"), DUI_T("system_14"));
    detail->SetAttribute(DUI_T("text_color"), kHint);
    detail->SetAttribute(DUI_T("height"), DUI_T("28"));
    panel->AddItem(detail);

    ui::Label* note = new ui::Label(this);
    note->SetText(DUI_T("扫描、选择网络和输入密码在「Wi-Fi」窗口里。"));
    note->SetAttribute(DUI_T("font"), DUI_T("system_12"));
    note->SetAttribute(DUI_T("text_color"), kHint);
    note->SetAttribute(DUI_T("height"), DUI_T("30"));
    panel->AddItem(note);

    ui::Button* open = new ui::Button(this);
    open->SetText(DUI_T("打开 Wi-Fi 设置…"));
    StyleButton(open);
    open->AttachClick([this](const ui::EventArgs&) {
        std::system("\"" POLLUX_BIN "/polluxdesk_wifi\" >/dev/null 2>&1 &");
        return true;
    });
    panel->AddItem(open);

    m_pStatus = new ui::Label(this);
    m_pStatus->SetText(DUI_T(""));
    m_pStatus->SetAttribute(DUI_T("font"), DUI_T("system_12"));
    m_pStatus->SetAttribute(DUI_T("text_color"), kHint);
    m_pStatus->SetAttribute(DUI_T("height"), DUI_T("30"));
    panel->AddItem(m_pStatus);
}

void SettingsForm::BuildAboutPanel(ui::VBox* panel)
{
    ui::Label* title = new ui::Label(this);
    title->SetText(DUI_T("关于 PolluxOS"));
    title->SetAttribute(DUI_T("font"), DUI_T("system_bold_18"));
    title->SetAttribute(DUI_T("text_color"), kText);
    title->SetAttribute(DUI_T("height"), DUI_T("34"));
    panel->AddItem(title);

    ui::Label* version = new ui::Label(this);
    version->SetText(DUI_T("版本  0.1.0"));
    version->SetAttribute(DUI_T("font"), DUI_T("system_16"));
    version->SetAttribute(DUI_T("text_color"), kText);
    version->SetAttribute(DUI_T("height"), DUI_T("32"));
    panel->AddItem(version);

    ui::Label* desc = new ui::Label(this);
    desc->SetText(DUI_T("PolluxOS 系统设置（简单演示版）"));
    desc->SetAttribute(DUI_T("font"), DUI_T("system_14"));
    desc->SetAttribute(DUI_T("text_color"), kHint);
    desc->SetAttribute(DUI_T("height"), DUI_T("28"));
    panel->AddItem(desc);
}

void SettingsForm::NotifyCompositor()
{
    std::system("pkill -USR1 -x polluxdesk-compositor 2>/dev/null");
}

void SettingsForm::ApplyWallpaper(const char* name)
{
    const char* home = std::getenv("HOME");
    if (home == nullptr) return;
    DString dir = DString(home) + DUI_T("/.config/polluxdesk");
    std::string mkdirCommand = "mkdir -p '" + std::string(dir.c_str()) + "'";
    std::system(mkdirCommand.c_str());
    DString path = dir + DUI_T("/settings.conf");
    FILE* file = std::fopen(path.c_str(), "a+");
    if (file == nullptr) return;
    std::fprintf(file, "wallpaper=%s\n", name);
    std::fclose(file);
    NotifyCompositor();
    if (m_pStatus != nullptr) m_pStatus->SetText(DUI_T("壁纸已应用"));
}

void SettingsForm::ApplyResolution(const char* value)
{
    const char* home = std::getenv("HOME");
    if (home == nullptr) return;
    DString dir = DString(home) + DUI_T("/.config/polluxdesk");
    std::string mkdirCommand = "mkdir -p '" + std::string(dir.c_str()) + "'";
    std::system(mkdirCommand.c_str());
    DString path = dir + DUI_T("/settings.conf");
    FILE* file = std::fopen(path.c_str(), "a+");
    if (file == nullptr) return;
    std::fprintf(file, "resolution=%s\n", value);
    std::fclose(file);
    NotifyCompositor();
    if (m_pStatus != nullptr) m_pStatus->SetText(DUI_T("分辨率已请求应用"));
}
