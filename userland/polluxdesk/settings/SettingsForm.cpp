#include "SettingsForm.h"

#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>

namespace {

const DString kBg = _T("#F2FFFFFF");
const DString kCard = _T("#F8FFFFFF");
const DString kBorder = _T("#559A9AA0");
const DString kText = _T("#FF1D1D1F");
const DString kHint = _T("#FF6E6E73");
const DString kAccent = _T("#FF0A84FF");

void StyleButton(ui::Button* button)
{
    button->SetAttribute(_T("font"), _T("system_14"));
    button->SetAttribute(_T("text_color"), kText);
    button->SetAttribute(_T("height"), _T("38"));
    button->SetAttribute(_T("width"), _T("190"));
    button->SetAttribute(_T("margin"), _T("4,4,4,4"));
    button->SetStateColor(ui::kControlStateNormal, _T("#FFFFFFFF"));
    button->SetStateColor(ui::kControlStateHot, _T("#220A84FF"));
    button->SetStateColorRound(ui::kControlStateNormal, ui::UiSize(9, 9), false);
    button->SetStateColorRound(ui::kControlStateHot, ui::UiSize(9, 9), false);
    button->SetAttribute(_T("border_round"), _T("9,9"));
    button->SetAttribute(_T("cursor_type"), _T("hand"));
}

void StyleSectionButton(ui::Button* button)
{
    button->SetAttribute(_T("font"), _T("system_14"));
    button->SetAttribute(_T("text_color"), kText);
    button->SetAttribute(_T("height"), _T("42"));
    button->SetAttribute(_T("width"), _T("stretch"));
    button->SetAttribute(_T("margin"), _T("8,2,8,2"));
    button->SetAttribute(_T("text_padding"), _T("14,0,0,0"));
    button->SetStateColor(ui::kControlStateNormal, _T("#00000000"));
    button->SetStateColor(ui::kControlStateHot, _T("#220A84FF"));
    button->SetStateColorRound(ui::kControlStateNormal, ui::UiSize(8, 8), false);
    button->SetStateColorRound(ui::kControlStateHot, ui::UiSize(8, 8), false);
    button->SetAttribute(_T("cursor_type"), _T("hand"));
}

} // namespace

SettingsForm::SettingsForm() = default;
SettingsForm::~SettingsForm() = default;

DString SettingsForm::GetSkinFolder() { return _T(""); }
DString SettingsForm::GetSkinFile() { return _T(""); }

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
    root->SetAttribute(_T("padding"), _T("22,22,22,22"));

    ui::Label* title = new ui::Label(this);
    title->SetText(_T("系统设置"));
    title->SetAttribute(_T("font"), _T("system_bold_24"));
    title->SetAttribute(_T("text_color"), kText);
    title->SetAttribute(_T("height"), _T("42"));
    root->AddItem(title);

    ui::HBox* content = new ui::HBox(this);
    content->SetAttribute(_T("height"), _T("stretch"));
    root->AddItem(content);

    // Left sidebar, macOS System Settings style.
    ui::VBox* sidebar = new ui::VBox(this);
    sidebar->SetAttribute(_T("width"), _T("210"));
    sidebar->SetBkColor(kCard);
    sidebar->SetBorderColor(kBorder);
    sidebar->SetAttribute(_T("border_size"), _T("1"));
    sidebar->SetAttribute(_T("border_round"), _T("12,12"));
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
    m_pPanel->SetAttribute(_T("width"), _T("stretch"));
    m_pPanel->SetAttribute(_T("padding"), _T("18,0,0,18"));
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
    wallpaper->SetText(_T("壁纸"));
    wallpaper->SetAttribute(_T("font"), _T("system_bold_18"));
    wallpaper->SetAttribute(_T("text_color"), kText);
    wallpaper->SetAttribute(_T("height"), _T("34"));
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
    resolution->SetText(_T("分辨率"));
    resolution->SetAttribute(_T("font"), _T("system_bold_18"));
    resolution->SetAttribute(_T("text_color"), kText);
    resolution->SetAttribute(_T("height"), _T("34"));
    resolution->SetAttribute(_T("margin"), _T("16,0,0,0"));
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
    m_pStatus->SetText(_T("修改会实时应用到桌面"));
    m_pStatus->SetAttribute(_T("font"), _T("system_12"));
    m_pStatus->SetAttribute(_T("text_color"), kHint);
    m_pStatus->SetAttribute(_T("height"), _T("30"));
    panel->AddItem(m_pStatus);
}

void SettingsForm::BuildNetworkPanel(ui::VBox* panel)
{
    ui::Label* title = new ui::Label(this);
    title->SetText(_T("网络"));
    title->SetAttribute(_T("font"), _T("system_bold_18"));
    title->SetAttribute(_T("text_color"), kText);
    title->SetAttribute(_T("height"), _T("34"));
    panel->AddItem(title);

    ui::Label* ethernet = new ui::Label(this);
    ethernet->SetText(_T("以太网  已连接"));
    ethernet->SetAttribute(_T("font"), _T("system_16"));
    ethernet->SetAttribute(_T("text_color"), kText);
    ethernet->SetAttribute(_T("height"), _T("32"));
    panel->AddItem(ethernet);

    ui::Label* ip = new ui::Label(this);
    ip->SetText(_T("IP 地址 192.168.0.104"));
    ip->SetAttribute(_T("font"), _T("system_14"));
    ip->SetAttribute(_T("text_color"), kHint);
    ip->SetAttribute(_T("height"), _T("28"));
    panel->AddItem(ip);

    ui::Label* mask = new ui::Label(this);
    mask->SetText(_T("子网掩码 255.255.255.0"));
    mask->SetAttribute(_T("font"), _T("system_14"));
    mask->SetAttribute(_T("text_color"), kHint);
    mask->SetAttribute(_T("height"), _T("28"));
    panel->AddItem(mask);

    ui::Button* refresh = new ui::Button(this);
    refresh->SetText(_T("刷新网络状态"));
    StyleButton(refresh);
    refresh->AttachClick([this](const ui::EventArgs&) {
        NotifyCompositor();
        if (m_pStatus != nullptr) {
            m_pStatus->SetText(_T("已通知 compositor 刷新状态"));
        }
        return true;
    });
    panel->AddItem(refresh);

    m_pStatus = new ui::Label(this);
    m_pStatus->SetText(_T("网络状态由系统自动管理"));
    m_pStatus->SetAttribute(_T("font"), _T("system_12"));
    m_pStatus->SetAttribute(_T("text_color"), kHint);
    m_pStatus->SetAttribute(_T("height"), _T("30"));
    panel->AddItem(m_pStatus);
}

void SettingsForm::BuildWifiPanel(ui::VBox* panel)
{
    ui::Label* title = new ui::Label(this);
    title->SetText(_T("Wi-Fi"));
    title->SetAttribute(_T("font"), _T("system_bold_18"));
    title->SetAttribute(_T("text_color"), kText);
    title->SetAttribute(_T("height"), _T("34"));
    panel->AddItem(title);

    ui::Label* status = new ui::Label(this);
    status->SetText(_T("Wi-Fi  已启用"));
    status->SetAttribute(_T("font"), _T("system_16"));
    status->SetAttribute(_T("text_color"), kText);
    status->SetAttribute(_T("height"), _T("32"));
    panel->AddItem(status);

    ui::Button* toggle = new ui::Button(this);
    toggle->SetText(_T("关闭 Wi-Fi"));
    StyleButton(toggle);
    toggle->AttachClick([this, toggle, status](const ui::EventArgs&) {
        bool enabled = status->GetText() == _T("Wi-Fi  已启用");
        status->SetText(enabled ? _T("Wi-Fi  已关闭") : _T("Wi-Fi  已启用"));
        toggle->SetText(enabled ? _T("启用 Wi-Fi") : _T("关闭 Wi-Fi"));
        return true;
    });
    panel->AddItem(toggle);

    ui::Label* network1 = new ui::Label(this);
    network1->SetText(_T("PolluxOS-5G"));
    network1->SetAttribute(_T("font"), _T("system_14"));
    network1->SetAttribute(_T("text_color"), kText);
    network1->SetAttribute(_T("height"), _T("30"));
    panel->AddItem(network1);

    ui::Label* network2 = new ui::Label(this);
    network2->SetText(_T("TP-LINK_2.4G"));
    network2->SetAttribute(_T("font"), _T("system_14"));
    network2->SetAttribute(_T("text_color"), kHint);
    network2->SetAttribute(_T("height"), _T("30"));
    panel->AddItem(network2);

    m_pStatus = new ui::Label(this);
    m_pStatus->SetText(_T("当前为界面演示，未修改真实 Wi-Fi 配置"));
    m_pStatus->SetAttribute(_T("font"), _T("system_12"));
    m_pStatus->SetAttribute(_T("text_color"), kHint);
    m_pStatus->SetAttribute(_T("height"), _T("30"));
    panel->AddItem(m_pStatus);
}

void SettingsForm::BuildAboutPanel(ui::VBox* panel)
{
    ui::Label* title = new ui::Label(this);
    title->SetText(_T("关于 PolluxOS"));
    title->SetAttribute(_T("font"), _T("system_bold_18"));
    title->SetAttribute(_T("text_color"), kText);
    title->SetAttribute(_T("height"), _T("34"));
    panel->AddItem(title);

    ui::Label* version = new ui::Label(this);
    version->SetText(_T("版本  0.1.0"));
    version->SetAttribute(_T("font"), _T("system_16"));
    version->SetAttribute(_T("text_color"), kText);
    version->SetAttribute(_T("height"), _T("32"));
    panel->AddItem(version);

    ui::Label* desc = new ui::Label(this);
    desc->SetText(_T("PolluxOS 系统设置（简单演示版）"));
    desc->SetAttribute(_T("font"), _T("system_14"));
    desc->SetAttribute(_T("text_color"), kHint);
    desc->SetAttribute(_T("height"), _T("28"));
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
    DString dir = DString(home) + _T("/.config/polluxdesk");
    std::string mkdirCommand = "mkdir -p '" + std::string(dir.c_str()) + "'";
    std::system(mkdirCommand.c_str());
    DString path = dir + _T("/settings.conf");
    FILE* file = std::fopen(path.c_str(), "a+");
    if (file == nullptr) return;
    std::fprintf(file, "wallpaper=%s\n", name);
    std::fclose(file);
    NotifyCompositor();
    if (m_pStatus != nullptr) m_pStatus->SetText(_T("壁纸已应用"));
}

void SettingsForm::ApplyResolution(const char* value)
{
    const char* home = std::getenv("HOME");
    if (home == nullptr) return;
    DString dir = DString(home) + _T("/.config/polluxdesk");
    std::string mkdirCommand = "mkdir -p '" + std::string(dir.c_str()) + "'";
    std::system(mkdirCommand.c_str());
    DString path = dir + _T("/settings.conf");
    FILE* file = std::fopen(path.c_str(), "a+");
    if (file == nullptr) return;
    std::fprintf(file, "resolution=%s\n", value);
    std::fclose(file);
    NotifyCompositor();
    if (m_pStatus != nullptr) m_pStatus->SetText(_T("分辨率已请求应用"));
}
