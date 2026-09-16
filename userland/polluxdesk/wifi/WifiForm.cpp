#include "WifiForm.h"

#include "PolluxPaths.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

const int kRefreshMs = 1000;
// The status is read from three programs; once a second would be three
// processes a second for a window that mostly sits still.
const int kStatusEveryTicks = 3;
// How long a join is given before it is called a failure.  A WPA handshake
// takes a couple of seconds; fifteen is already generous.
const int kJoinTimeoutSeconds = 15;
// Ticks to keep reading the status every second after something changed, so
// what the window shows catches up with what was just done.
const int kSettleTicks = 4;

U8String Num(long long value)
{
    return ui::StringUtil::Printf("%lld", value);
}

int SignalLevel(int dbm)
{
    if (dbm >= -55) return 4;
    if (dbm >= -65) return 3;
    if (dbm >= -75) return 2;
    return 1;
}

}  // namespace

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------
WifiForm::WifiForm(const std::string& preselectSsid)
    : m_preselectSsid(preselectSsid)
{
}

WifiForm::~WifiForm()
{
    if (m_timerId > 0) {
        ui::GlobalManager::Instance().Timer().RemoveTimer(m_timerId);
        m_timerId = 0;
    }
    // The scan thread touches this object's members, so it has to be finished
    // with before the object is.
    if (m_scanThread.joinable()) {
        m_scanThread.join();
    }
}

U8String WifiForm::GetSkinFolder() { return ""; }
U8String WifiForm::GetSkinFile()   { return ""; }

void WifiForm::GetCreateWindowAttributes(ui::WindowCreateAttributes& attrs)
{
    attrs.m_bInitSizeDefined = true;
    attrs.m_szInitSize.cx = 640;
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

void WifiForm::OnInitWindow()
{
    SetShadowAttached(false);

    m_settings = pollux::Load();
    m_settingsMtime = pollux::MtimeNs(pollux::ConfigPath());
    ApplyAppearance();

    BuildUi();
    BaseClass::OnInitWindow();

    RefreshStatus();
    StartScan();

    constexpr int32_t kRepeatForSession = 0x7FFFFFFF;
    m_timerId = ui::GlobalManager::Instance().Timer().AddTimer(GetWeakFlag(), [this]() {
        OnTick();
    }, kRefreshMs, kRepeatForSession);
}

void WifiForm::OnTick()
{
    PollSettings();
    if (m_uiDirty) {
        m_uiDirty = false;
        RebuildUi();
    }

    CollectScan();

    static int tick = 0;
    // Every tick while a join is running or a change is still settling -- that
    // is when the answer is worth the processes -- and every few ticks the
    // rest of the time, for a window that mostly sits still.
    if (m_joinSecondsLeft > 0 || m_settleTicks > 0) {
        if (m_settleTicks > 0) {
            --m_settleTicks;
        }
        RefreshStatus();
    } else if (++tick % kStatusEveryTicks == 0) {
        RefreshStatus();
    }

    // A join is watched to its end: wpa_supplicant accepting the network says
    // nothing about whether the handshake will work out.
    if (!m_joiningSsid.empty()) {
        if (m_status.connected && m_status.ssid == m_joiningSsid) {
            U8String note = U8String("已连接到 ") + U8String(m_joiningSsid.c_str());
            // Only now is this worth keeping: written any earlier, a mistyped
            // password would have replaced the saved key of a network that
            // used to work, and outlived the attempt that mistyped it.
            std::string saveError;
            if (!m_wifi.SaveConfig(saveError)) {
                note += "（没能写入配置，重启后需要重连）";
            }
            SetHint(note, false);
            m_joiningSsid.clear();
            m_joinSecondsLeft = 0;
            // The bar is rebuilt from the status, and a network that is now
            // connected has no password field -- so this may be a field that
            // no longer exists.
            if (m_pPassword != nullptr) {
                m_pPassword->SetText("");
            }
            RebuildNetworkRows();
            UpdateConnectBar();
        } else if (--m_joinSecondsLeft <= 0) {
            // Only a handshake that was tried and refused says something about
            // the password.  Everything else -- still scanning, still
            // associating -- means the attempt has not finished, and calling
            // that a failure would be a guess, so the state is named instead.
            U8String reason = "连接超时";
            if (m_status.state == "4WAY_HANDSHAKE" ||
                m_status.state == "GROUP_HANDSHAKE") {
                reason = "连接失败：密码可能不正确";
            } else if (!m_status.state.empty()) {
                reason = U8String("还没连上（") +
                         U8String(m_status.state.c_str()) + "），可以再试一次";
            }
            SetHint(reason, true);
            m_joiningSsid.clear();
        }
    }
}

void WifiForm::PollSettings()
{
    const unsigned long long mtime = pollux::MtimeNs(pollux::ConfigPath());
    if (mtime == m_settingsMtime) {
        return;
    }
    m_settingsMtime = mtime;
    m_settings = pollux::Load();
    m_uiDirty = true;
}

// ---------------------------------------------------------------------------
// Appearance
// ---------------------------------------------------------------------------
void WifiForm::ApplyAppearance()
{
    const bool dark = pollux::IsDark(m_settings);
    m_pal.accent = U8String(pollux::AccentHex(m_settings));
    m_pal.textOnAccent = "#FFFFFFFF";

    if (dark) {
        m_pal.windowBg     = "#FF1C1C1E";
        m_pal.cardBg       = "#FF2C2C2E";
        m_pal.cardBorder   = "#26FFFFFF";
        m_pal.rowHot       = "#1AFFFFFF";
        m_pal.rowSelected  = "#330A84FF";
        m_pal.hairline     = "#26FFFFFF";
        m_pal.textStrong   = "#FFF5F5F7";
        m_pal.textBody     = "#FFD8D8DC";
        m_pal.textHint     = "#FF98989D";
        m_pal.danger       = "#FFFF453A";
        m_pal.fieldBg      = "#FF3A3A3C";
        m_pal.fieldBorder  = "#33FFFFFF";
        m_pal.track        = "#33FFFFFF";
    } else {
        m_pal.windowBg     = "#FFF2F2F7";
        m_pal.cardBg       = "#FFFFFFFF";
        m_pal.cardBorder   = "#22000000";
        m_pal.rowHot       = "#14000000";
        m_pal.rowSelected  = "#260A84FF";
        m_pal.hairline     = "#22000000";
        m_pal.textStrong   = "#FF1D1D1F";
        m_pal.textBody     = "#FF3A3A3C";
        m_pal.textHint     = "#FF8E8E93";
        m_pal.danger       = "#FFFF3B30";
        m_pal.fieldBg      = "#FFFFFFFF";
        m_pal.fieldBorder  = "#33000000";
        m_pal.track        = "#1F000000";
    }
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
void WifiForm::SetRadius(ui::Control* pControl, int radius, bool interactive)
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

ui::Label* WifiForm::AddLabel(ui::Box* pParent, const U8String& text,
                              const U8String& font, const U8String& colour)
{
    ui::Label* pLabel = new ui::Label(this);
    pLabel->SetText(text);
    pLabel->SetAttribute("font", font);
    pLabel->SetStateTextColor(ui::kControlStateNormal, colour);
    pLabel->SetAttribute("text_align", "left,vcenter");
    pLabel->SetMouseEnabled(false);
    pParent->AddItem(pLabel);
    return pLabel;
}

void WifiForm::BuildUi()
{
    m_pScanButton = nullptr;
    m_pStatusCard = nullptr;
    m_pStatusHeadline = nullptr;
    m_pStatusSsid = nullptr;
    m_pStatusDetail = nullptr;
    m_pStatusWarning = nullptr;
    m_pNetworkList = nullptr;
    m_pNetworkEmpty = nullptr;
    m_rowButtons.clear();
    m_pConnectBar = nullptr;
    m_connectBarShape.clear();
    m_pPassword = nullptr;
    m_pPasswordLabel = nullptr;
    m_pShowButton = nullptr;
    m_pConnectButton = nullptr;
    m_pForgetButton = nullptr;
    m_pHintLabel = nullptr;
    m_pSetupLabel = nullptr;

    ui::VBox* pRoot = new ui::VBox(this);
    pRoot->SetBkColor(m_pal.windowBg);
    pRoot->SetBorderColor("#00000000");
    pRoot->SetAttribute("border_size", "0");
    pRoot->SetAttribute("padding", "0,0,0,0");

    BuildHeader(pRoot);
    BuildStatusCard(pRoot);
    BuildNetworkList(pRoot);
    BuildConnectBar(pRoot);

    AttachBox(pRoot);
}

void WifiForm::BuildHeader(ui::VBox* pRoot)
{
    ui::HBox* pHeader = new ui::HBox(this);
    pHeader->SetAttribute("height", "56");
    pHeader->SetBkColor(m_pal.windowBg);
    pHeader->SetAttribute("padding", "16,0,16,0");
    pHeader->SetAttribute("child_align", "vcenter");
    pRoot->AddItem(pHeader);

    ui::Label* pTitle = AddLabel(pHeader, "Wi-Fi", "system_bold_18",
                                 m_pal.textStrong);
    pTitle->SetAttribute("width", "stretch");

    // A scan takes a second or two; this is the button that starts one.
    m_pScanButton = new ui::Button(this);
    m_pScanButton->SetText("重新扫描");
    m_pScanButton->SetAttribute("font", "system_12");
    m_pScanButton->SetAttribute("width", "88");
    m_pScanButton->SetAttribute("height", "28");
    m_pScanButton->SetAttribute("text_align", "hcenter,vcenter");
    m_pScanButton->SetAttribute("cursor_type", "hand");
    m_pScanButton->SetStateColor(ui::kControlStateNormal, m_pal.rowHot);
    m_pScanButton->SetStateColor(ui::kControlStateHot, m_pal.rowSelected);
    m_pScanButton->SetStateTextColor(ui::kControlStateNormal, m_pal.textBody);
    SetRadius(m_pScanButton, 8, true);
    m_pScanButton->AttachClick([this](const ui::EventArgs& /*args*/) {
        StartScan();
        return true;
    });
    pHeader->AddItem(m_pScanButton);
}

void WifiForm::BuildStatusCard(ui::VBox* pRoot)
{
    m_pStatusCard = new ui::VBox(this);
    m_pStatusCard->SetAttribute("height", "104");
    m_pStatusCard->SetAttribute("width", "stretch");
    m_pStatusCard->SetAttribute("margin", "16,0,16,0");
    m_pStatusCard->SetAttribute("padding", "14,10,14,10");
    m_pStatusCard->SetBkColor(m_pal.cardBg);
    m_pStatusCard->SetBorderColor(m_pal.cardBorder);
    m_pStatusCard->SetAttribute("border_size", "1");
    SetRadius(m_pStatusCard, 10, false);
    pRoot->AddItem(m_pStatusCard);

    m_pStatusHeadline = AddLabel(m_pStatusCard, "", "system_12",
                                 m_pal.textHint);
    m_pStatusHeadline->SetAttribute("height", "18");
    m_pStatusHeadline->SetAttribute("width", "stretch");

    m_pStatusSsid = AddLabel(m_pStatusCard, "", "system_bold_18",
                             m_pal.textStrong);
    m_pStatusSsid->SetAttribute("height", "28");
    m_pStatusSsid->SetAttribute("width", "stretch");

    m_pStatusDetail = AddLabel(m_pStatusCard, "", "system_12",
                               m_pal.textHint);
    m_pStatusDetail->SetAttribute("height", "18");
    m_pStatusDetail->SetAttribute("width", "stretch");

    m_pStatusWarning = AddLabel(m_pStatusCard, "", "system_12",
                                m_pal.danger);
    m_pStatusWarning->SetAttribute("height", "20");
    m_pStatusWarning->SetAttribute("width", "stretch");
}

void WifiForm::BuildNetworkList(ui::VBox* pRoot)
{
    ui::Label* pHeading = AddLabel(pRoot, "网络", "system_12",
                                   m_pal.textHint);
    pHeading->SetAttribute("height", "24");
    pHeading->SetAttribute("width", "stretch");
    pHeading->SetAttribute("margin", "18,10,0,2");

    m_pNetworkList = new ui::VScrollBox(this);
    m_pNetworkList->SetAttribute("height", "stretch");
    m_pNetworkList->SetAttribute("width", "stretch");
    m_pNetworkList->SetAttribute("padding", "10,0,10,0");
    m_pNetworkList->SetAttribute("vscrollbar", "true");
    pRoot->AddItem(m_pNetworkList);

    m_pNetworkEmpty = AddLabel(m_pNetworkList, "正在扫描…", "system_12",
                               m_pal.textHint);
    m_pNetworkEmpty->SetAttribute("height", "28");
    m_pNetworkEmpty->SetAttribute("width", "stretch");
    m_pNetworkEmpty->SetAttribute("margin", "8,0,0,0");
}

void WifiForm::BuildConnectBar(ui::VBox* pRoot)
{
    ui::Control* pHairline = new ui::Control(this);
    pHairline->SetAttribute("height", "1");
    pHairline->SetAttribute("width", "stretch");
    pHairline->SetBkColor(m_pal.hairline);
    pHairline->SetMouseEnabled(false);
    pRoot->AddItem(pHairline);

    // Fixed height, and its *contents* are what change: dui lays a control out
    // once, so a bar that grew and shrank would keep the size it was built
    // with.  Filling it again is safe from a click handler because the rows
    // that are clicked live in the list above, not in here.
    m_pConnectBar = new ui::VBox(this);
    m_pConnectBar->SetAttribute("height", "58");
    m_pConnectBar->SetAttribute("width", "stretch");
    m_pConnectBar->SetAttribute("padding", "16,0,16,0");
    m_pConnectBar->SetAttribute("child_align", "vcenter");
    pRoot->AddItem(m_pConnectBar);

    m_pPassword = nullptr;
    m_pPasswordLabel = nullptr;
    m_pConnectButton = nullptr;
    m_connectBarShape.clear();

    // Two lines: what just happened, and -- in read-only mode -- the command
    // that fixes that.  The command is long enough to need a line of its own.
    ui::VBox* pHints = new ui::VBox(this);
    pHints->SetAttribute("height", "46");
    pHints->SetAttribute("width", "stretch");
    pHints->SetAttribute("padding", "18,2,16,2");
    pRoot->AddItem(pHints);

    m_pHintLabel = AddLabel(pHints, "", "system_12", m_pal.textHint);
    m_pHintLabel->SetAttribute("height", "22");
    m_pHintLabel->SetAttribute("width", "stretch");

    m_pSetupLabel = AddLabel(pHints, "", "system_12", m_pal.textBody);
    m_pSetupLabel->SetAttribute("height", "20");
    m_pSetupLabel->SetAttribute("width", "stretch");
}

void WifiForm::FillConnectBar()
{
    if (m_pConnectBar == nullptr) {
        return;
    }
    m_pConnectBar->RemoveAllItems();
    m_pPassword = nullptr;
    m_pPasswordLabel = nullptr;
    m_pShowButton = nullptr;
    m_pConnectButton = nullptr;
    m_pForgetButton = nullptr;

    const wifi::Network* network = nullptr;
    for (const wifi::Network& candidate : m_networks) {
        if (candidate.ssid == m_selectedSsid) {
            network = &candidate;
            break;
        }
    }
    const bool connectedHere = network != nullptr && !m_status.ssid.empty() &&
                               network->ssid == m_status.ssid;

    ui::HBox* pRow = new ui::HBox(this);
    pRow->SetAttribute("height", "34");
    pRow->SetAttribute("width", "stretch");
    pRow->SetAttribute("child_align", "vcenter");
    m_pConnectBar->AddItem(pRow);

    if (!m_status.haveControl) {
        ui::Label* pHint = AddLabel(pRow, m_status.controlDenied
                                              ? "只读模式：当前用户无权使用控制接口，见下方命令"
                                              : "只读模式：控制接口未启用，见下方命令",
                                    "system_12", m_pal.textHint);
        pHint->SetAttribute("width", "stretch");
        m_connectBarShape = "readonly";
        return;
    }
    if (network == nullptr) {
        ui::Label* pHint = AddLabel(pRow, "点击上面的网络，输入密码后连接",
                                    "system_12", m_pal.textHint);
        pHint->SetAttribute("width", "stretch");
        m_connectBarShape = "none";
        return;
    }

    const bool needPassword = network->secured && !connectedHere;

    if (needPassword) {
        m_pPasswordLabel = AddLabel(pRow, "密码", "system_14",
                                    m_pal.textBody);
        m_pPasswordLabel->SetAttribute("width", "44");

        // The password goes into a real text field, which is the whole reason
        // this window exists: the desktop shell never receives the keyboard.
        m_pPassword = new ui::RichEdit(this);
        m_pPassword->SetAttribute("width", "stretch");
        m_pPassword->SetAttribute("height", "32");
        m_pPassword->SetAttribute("margin", "0,0,10,0");
        m_pPassword->SetFontId("system_14");
        m_pPassword->SetTextColor(m_pal.textStrong);
        m_pPassword->SetBkColor(m_pal.fieldBg);
        m_pPassword->SetBorderColor(m_pal.fieldBorder);
        m_pPassword->SetAttribute("border_size", "1");
        // Text is laid out from the top of this rect, so a 14px font in a 32px
        // field sat against the upper edge; the top inset is what moves it --
        // and the caret, which is measured from the same rect -- down to the
        // middle.  The bottom inset stays 0 so the line still has room to sit
        // in: a rect shorter than the line is what would clip it.
        m_pPassword->SetTextPadding(ui::UiPadding(10, 6, 10, 0), false);
        m_pPassword->SetPasswordMode(true);
        m_pPassword->SetLimitText(64);
        SetRadius(m_pPassword, 8, false);
        m_pPassword->SetShowPassword(m_passwordVisible);
        m_pPassword->AttachReturn([this](const ui::EventArgs& /*args*/) {
            DoConnect();
            return true;
        });
        pRow->AddItem(m_pPassword);

        // Reveal what was typed.  A wrong password is impossible to check
        // behind dots, and there is no physical keyboard shortcut to reach
        // for here.
        m_pShowButton = new ui::Button(this);
        m_pShowButton->SetText(m_passwordVisible ? "隐藏" : "显示");
        m_pShowButton->SetAttribute("font", "system_12");
        m_pShowButton->SetAttribute("width", "56");
        m_pShowButton->SetAttribute("height", "32");
        m_pShowButton->SetAttribute("margin", "0,0,10,0");
        m_pShowButton->SetAttribute("text_align", "hcenter,vcenter");
        m_pShowButton->SetAttribute("cursor_type", "hand");
        // Without this, clicking 显示 moves the caret out of the field and the
        // next keystroke goes to the button.
        m_pShowButton->SetNoFocus();
        m_pShowButton->SetStateColor(ui::kControlStateNormal, m_pal.rowHot);
        m_pShowButton->SetStateColor(ui::kControlStateHot, m_pal.rowSelected);
        m_pShowButton->SetStateTextColor(ui::kControlStateNormal, m_pal.textBody);
        SetRadius(m_pShowButton, 8, true);
        m_pShowButton->AttachClick([this](const ui::EventArgs& /*args*/) {
            m_passwordVisible = !m_passwordVisible;
            if (m_pPassword != nullptr) {
                m_pPassword->SetShowPassword(m_passwordVisible);
            }
            if (m_pShowButton != nullptr) {
                m_pShowButton->SetText(m_passwordVisible ? "隐藏"
                                                         : "显示");
            }
            return true;
        });
        pRow->AddItem(m_pShowButton);
    } else {
        ui::Label* pNote = AddLabel(pRow,
                                    connectedHere
                                        ? "这是当前连接的网络"
                                        : (network->saved
                                               ? "已保存的网络"
                                               : "开放网络，无需密码"),
                                    "system_12", m_pal.textHint);
        pNote->SetAttribute("width", "stretch");
    }

    // A saved network can be dropped without the password being retyped:
    // wpa_supplicant throws the entry -- key included -- away.
    if (network->saved) {
        m_pForgetButton = new ui::Button(this);
        m_pForgetButton->SetText("忽略此网络");
        m_pForgetButton->SetAttribute("font", "system_12");
        m_pForgetButton->SetAttribute("width", "92");
        m_pForgetButton->SetAttribute("height", "32");
        m_pForgetButton->SetAttribute("margin", "0,0,10,0");
        m_pForgetButton->SetAttribute("text_align", "hcenter,vcenter");
        m_pForgetButton->SetAttribute("cursor_type", "hand");
        m_pForgetButton->SetStateColor(ui::kControlStateNormal, m_pal.rowHot);
        m_pForgetButton->SetStateColor(ui::kControlStateHot, m_pal.danger);
        m_pForgetButton->SetStateTextColor(ui::kControlStateNormal, m_pal.textBody);
        m_pForgetButton->SetStateTextColor(ui::kControlStateHot, m_pal.danger);
        SetRadius(m_pForgetButton, 8, true);
        m_pForgetButton->AttachClick([this](const ui::EventArgs& /*args*/) {
            DoForget();
            return true;
        });
        pRow->AddItem(m_pForgetButton);
    }

    m_pConnectButton = new ui::Button(this);
    m_pConnectButton->SetText(connectedHere ? "断开" : "连接");
    m_pConnectButton->SetAttribute("font", "system_14");
    m_pConnectButton->SetAttribute("width", "88");
    m_pConnectButton->SetAttribute("height", "32");
    m_pConnectButton->SetAttribute("text_align", "hcenter,vcenter");
    m_pConnectButton->SetAttribute("cursor_type", "hand");
    m_pConnectButton->SetStateColor(ui::kControlStateNormal,
                                    connectedHere ? m_pal.danger : m_pal.accent);
    m_pConnectButton->SetStateColor(ui::kControlStateHot,
                                    connectedHere ? m_pal.danger : m_pal.accent);
    m_pConnectButton->SetStateTextColor(ui::kControlStateNormal, m_pal.textOnAccent);
    m_pConnectButton->SetStateColor(ui::kControlStateDisabled, m_pal.track);
    m_pConnectButton->SetStateTextColor(ui::kControlStateDisabled, m_pal.textHint);
    SetRadius(m_pConnectButton, 8, true);
    m_pConnectButton->AttachClick([this](const ui::EventArgs& /*args*/) {
        DoConnect();
        return true;
    });
    pRow->AddItem(m_pConnectButton);

    m_connectBarShape = connectedHere ? "connected"
                                      : (needPassword ? "secure" : "open");

    // Straight to the field: a network was just picked, and the password is
    // the only thing left to do.
    if (m_pPassword != nullptr) {
        m_pPassword->SetFocus();
    }
}

void WifiForm::RebuildUi()
{
    ApplyAppearance();
    BuildUi();
    RefreshStatus();
    RebuildNetworkRows();
    UpdateConnectBar();
    // BuildUi made a fresh hint label; put the current message back into it.
    SetHint(m_hint, m_hintBad);
    InvalidateAll();
}

// ---------------------------------------------------------------------------
// Scanning
// ---------------------------------------------------------------------------
void WifiForm::StartScan()
{
    if (m_scanRunning) {
        return;
    }
    if (m_scanThread.joinable()) {
        m_scanThread.join();
    }
    {
        std::lock_guard<std::mutex> lock(m_scanMutex);
        m_scanOutcome.done = false;
    }
    if (m_pNetworkEmpty != nullptr) {
        m_pNetworkEmpty->SetText("正在扫描…");
        m_pNetworkEmpty->SetAttribute("height",
                                      m_networks.empty() ? "28" : "0");
    }
    m_scanRunning = true;
    m_scanThread = std::thread([this]() {
        ScanOutcome outcome;
        outcome.ok = m_wifi.Scan(outcome.networks, outcome.error);
        std::lock_guard<std::mutex> lock(m_scanMutex);
        m_scanOutcome = std::move(outcome);
        m_scanOutcome.done = true;
    });
}

void WifiForm::CollectScan()
{
    ScanOutcome outcome;
    {
        std::lock_guard<std::mutex> lock(m_scanMutex);
        if (!m_scanRunning || !m_scanOutcome.done) {
            return;
        }
        outcome = std::move(m_scanOutcome);
        m_scanOutcome = ScanOutcome();
    }
    if (m_scanThread.joinable()) {
        m_scanThread.join();
    }
    m_scanRunning = false;

    if (outcome.ok) {
        m_networks = std::move(outcome.networks);
        // Preselect: whatever we are on, or the only network there is.
        if (m_selectedSsid.empty() && !m_status.ssid.empty()) {
            m_selectedSsid = m_status.ssid;
        }
        for (const wifi::Network& network : m_networks) {
            if (network.connected) {
                m_selectedSsid = network.ssid;
            }
        }
        // The menu bar's popover names a network on the command line when it
        // cannot type the password itself; open with that one picked.
        if (!m_preselectSsid.empty()) {
            for (const wifi::Network& network : m_networks) {
                if (network.ssid == m_preselectSsid) {
                    SelectNetwork(network.ssid);
                    break;
                }
            }
            m_preselectSsid.clear();
        }
    } else {
        m_networks.clear();
    }

    RebuildNetworkRows();
    if (!outcome.ok) {
        SetHint(U8String(outcome.error.c_str()), true);
    } else if (m_pNetworkEmpty != nullptr) {
        m_pNetworkEmpty->SetText(m_networks.empty() ? "没有扫描到网络" : "");
        m_pNetworkEmpty->SetAttribute("height",
                                      m_networks.empty() ? "28" : "0");
    }
    UpdateConnectBar();
}

ui::HBox* WifiForm::MakeSignalBars(ui::Window* pWindow, int dbm, bool active)
{
    // Four bars, the number of them lit telling the strength at a glance.
    const int level = SignalLevel(dbm);
    ui::HBox* pBars = new ui::HBox(pWindow);
    pBars->SetAttribute("width", "26");
    pBars->SetAttribute("height", "16");
    pBars->SetAttribute("margin", "0,0,10,0");
    pBars->SetAttribute("child_align", "vbottom");
    pBars->SetMouseEnabled(false);

    const int heights[] = { 4, 7, 10, 13 };
    for (int i = 0; i < 4; ++i) {
        ui::Control* pBar = new ui::Control(pWindow);
        pBar->SetAttribute("width", "4");
        pBar->SetAttribute("height", Num(heights[i]));
        pBar->SetAttribute("margin", "0,0,2,0");
        const bool lit = (i < level);
        pBar->SetBkColor(lit ? m_pal.accent
                             : (active ? m_pal.textHint : m_pal.track));
        SetRadius(pBar, 2, false);
        pBar->SetMouseEnabled(false);
        pBars->AddItem(pBar);
    }
    return pBars;
}

void WifiForm::RebuildNetworkRows()
{
    if (m_pNetworkList == nullptr) {
        return;
    }
    m_pNetworkList->RemoveAllItems();
    m_rowButtons.clear();

    m_pNetworkEmpty = AddLabel(m_pNetworkList, "", "system_12",
                               m_pal.textHint);
    m_pNetworkEmpty->SetAttribute("height",
                                  m_networks.empty() ? "28" : "0");
    m_pNetworkEmpty->SetAttribute("width", "stretch");
    m_pNetworkEmpty->SetAttribute("margin", "8,0,0,0");

    for (const wifi::Network& network : m_networks) {
        const std::string ssid = network.ssid;
        const bool connected = network.connected ||
                               (!m_status.ssid.empty() && ssid == m_status.ssid);

        ui::ButtonHBox* pRow = new ui::ButtonHBox(this);
        pRow->SetAttribute("height", "38");
        pRow->SetAttribute("width", "stretch");
        pRow->SetAttribute("padding", "8,0,8,0");
        pRow->SetAttribute("cursor_type", "hand");
        pRow->SetAttribute("child_align", "vcenter");
        pRow->SetStateColor(ui::kControlStateNormal, "#00000000");
        pRow->SetStateColor(ui::kControlStateHot, m_pal.rowHot);
        SetRadius(pRow, 8, true);
        pRow->AttachClick([this, ssid](const ui::EventArgs& /*args*/) {
            SelectNetwork(ssid);
            return true;
        });
        m_pNetworkList->AddItem(pRow);
        m_rowButtons.push_back(pRow);

        pRow->AddItem(MakeSignalBars(this, network.signalDbm, network.saved));

        ui::Label* pName = AddLabel(pRow, U8String(ssid.c_str()), "system_14",
                                    m_pal.textStrong);
        pName->SetAttribute("width", "stretch");

        ui::Label* pSecurity = AddLabel(pRow, U8String(network.security.c_str()),
                                        "system_12", m_pal.textHint);
        pSecurity->SetAttribute("width", "64");
        pSecurity->SetAttribute("text_align", "right,vcenter");

        ui::Label* pState = AddLabel(pRow,
                                     connected ? "已连接"
                                               : (network.saved ? "已保存"
                                                                : ""),
                                     "system_12",
                                     connected ? m_pal.accent : m_pal.textHint);
        pState->SetAttribute("width", "60");
        pState->SetAttribute("text_align", "right,vcenter");
    }

    UpdateSelection();
}

void WifiForm::SelectNetwork(const std::string& ssid)
{
    m_selectedSsid = ssid;
    UpdateSelection();
    UpdateConnectBar();

    const wifi::Network* network = nullptr;
    for (const wifi::Network& candidate : m_networks) {
        if (candidate.ssid == ssid) {
            network = &candidate;
            break;
        }
    }
    // The caret belongs in the field whenever there is one: a saved network's
    // key can be replaced, and typing into a field that was not focused went
    // nowhere at all.
    if (m_pPassword != nullptr) {
        m_pPassword->SetFocus();
    }
    if (network != nullptr && network->secured && !network->saved &&
        m_status.ssid != ssid) {
        m_pPassword->SetText("");
        SetHint("输入 Wi-Fi 密码后点连接（也可以直接按回车）", false);
    } else if (network != nullptr && network->saved && m_status.ssid != ssid) {
        SetHint("已保存的网络，直接点连接即可", false);
    }
}

void WifiForm::UpdateSelection()
{
    for (size_t i = 0; i < m_rowButtons.size() && i < m_networks.size(); ++i) {
        const bool selected = m_networks[i].ssid == m_selectedSsid;
        m_rowButtons[i]->SetStateColor(ui::kControlStateNormal,
                                       selected ? m_pal.rowSelected
                                                : "#00000000");
        m_rowButtons[i]->SetStateColor(ui::kControlStateHot,
                                       selected ? m_pal.rowSelected : m_pal.rowHot);
    }
}

void WifiForm::UpdateConnectBar()
{
    const wifi::Network* network = nullptr;
    for (const wifi::Network& candidate : m_networks) {
        if (candidate.ssid == m_selectedSsid) {
            network = &candidate;
            break;
        }
    }
    const bool connectedHere = network != nullptr && !m_status.ssid.empty() &&
                               network->ssid == m_status.ssid;

    U8String shape;
    if (!m_status.haveControl) {
        shape = "readonly";
    } else if (network == nullptr) {
        shape = "none";
    } else if (connectedHere) {
        shape = "connected";
    } else {
        shape = network->secured ? "secure" : "open";
    }

    // Only when what the bar should contain changes: rebuilding it throws away
    // whatever was typed into the password field, and a rescan must not do
    // that while someone is in the middle of typing.
    if (shape != m_connectBarShape) {
        FillConnectBar();
    }
}

// ---------------------------------------------------------------------------
// Connecting
// ---------------------------------------------------------------------------
void WifiForm::DoConnect()
{
    const wifi::Network* network = nullptr;
    for (const wifi::Network& candidate : m_networks) {
        if (candidate.ssid == m_selectedSsid) {
            network = &candidate;
            break;
        }
    }
    if (network == nullptr) {
        SetHint("先在上面选一个网络", true);
        return;
    }
    if (!m_status.haveControl) {
        SetHint("wpa_supplicant 控制接口不可用，见上方提示", true);
        return;
    }
    if (!m_status.ssid.empty() && network->ssid == m_status.ssid) {
        DoDisconnect();
        return;
    }

    U8String typed = m_pPassword != nullptr ? m_pPassword->GetText() : U8String();
    std::string psk(typed.c_str());
    if (network->secured && !network->saved && psk.empty()) {
        SetHint("这个网络需要密码", true);
        m_pPassword->SetFocus();
        return;
    }

    std::string error;
    if (!m_wifi.Connect(network->ssid, psk, network->secured, error)) {
        SetHint(U8String((std::string("无法连接：") + error).c_str()), true);
        return;
    }

    m_joiningSsid = network->ssid;
    m_joinSecondsLeft = kJoinTimeoutSeconds;
    SetHint(U8String((std::string("正在连接 ") + network->ssid + " …").c_str()), false);
}

void WifiForm::DoDisconnect()
{
    std::string error;
    if (!m_wifi.Disconnect(error)) {
        SetHint(U8String((std::string("无法断开：") + error).c_str()), true);
        return;
    }
    // Take the daemon at its word.  It has accepted the disconnect, so the
    // link is going down and the address with it -- but reading that back
    // takes a moment, and until it lands the bar still offers 断开 where the
    // thing to do next is connect.  Showing the state we asked for keeps the
    // button honest, and the next poll corrects it if the daemon disagrees.
    m_status.connected = false;
    m_status.ssid.clear();
    m_status.ipAddress.clear();
    m_status.bssid.clear();
    m_status.signalDbm = 0;
    m_status.state = "DISCONNECTED";
    m_settleTicks = kSettleTicks;
    SetHint("已断开。选一个网络可以重新连接。", false);
    ShowStatus();
    UpdateConnectBar();
}

void WifiForm::DoForget()
{
    if (m_selectedSsid.empty()) {
        return;
    }
    std::string error;
    if (!m_wifi.Forget(m_selectedSsid, error)) {
        SetHint(U8String((std::string("无法忽略：") + error).c_str()), true);
        return;
    }
    // Dropping the entry disconnects the radio too when it was the one in
    // use, and the row loses its 已保存 mark -- a rescan repaints both.
    if (m_selectedSsid == m_status.ssid) {
        // Same reasoning as a disconnect: the daemon has taken the network
        // away, so the connection it was carrying is going with it.
        m_status.connected = false;
        m_status.ssid.clear();
        m_status.ipAddress.clear();
        m_status.bssid.clear();
        m_status.signalDbm = 0;
        m_status.state = "DISCONNECTED";
        ShowStatus();
    }
    m_settleTicks = kSettleTicks;
    SetHint(U8String((std::string("已忽略 ") + m_selectedSsid).c_str()), false);
    m_selectedSsid.clear();
    StartScan();
    RebuildNetworkRows();
    UpdateConnectBar();
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------
void WifiForm::RefreshStatus()
{
    m_wifi.ReadStatus(m_status);

    // Without a control socket there is no signal_poll; the scan still knows
    // how strong the network we are on is.
    if (m_status.signalDbm == 0 && !m_status.ssid.empty()) {
        for (const wifi::Network& network : m_networks) {
            if (network.ssid == m_status.ssid) {
                m_status.signalDbm = network.signalDbm;
                break;
            }
        }
    }

    ShowStatus();
}

/** Paint the card from m_status, without reading anything back.  Separated
 *  from the reading so an action can show its result at once instead of
 *  waiting up to a full polling interval for the world to agree: a disconnect
 *  that has been accepted but not yet reflected left the button saying 断开
 *  for seconds, and the next click on it -- which looked like 连接 -- was a
 *  second disconnect. */
void WifiForm::ShowStatus()
{
    if (m_pStatusHeadline != nullptr) {
        if (m_wifi.Interface().empty()) {
            m_pStatusHeadline->SetText("没有无线网卡");
            m_pStatusHeadline->SetStateTextColor(ui::kControlStateNormal, m_pal.danger);
        } else if (m_status.connected) {
            m_pStatusHeadline->SetText("已连接");
            m_pStatusHeadline->SetStateTextColor(ui::kControlStateNormal, m_pal.accent);
        } else if (!m_joiningSsid.empty()) {
            m_pStatusHeadline->SetText("正在连接…");
            m_pStatusHeadline->SetStateTextColor(ui::kControlStateNormal, m_pal.textBody);
        } else {
            m_pStatusHeadline->SetText("未连接");
            m_pStatusHeadline->SetStateTextColor(ui::kControlStateNormal, m_pal.textHint);
        }
    }
    if (m_pStatusSsid != nullptr) {
        m_pStatusSsid->SetText(m_status.ssid.empty() ? "—"
                                                     : U8String(m_status.ssid.c_str()));
    }
    if (m_pStatusDetail != nullptr) {
        U8String detail;
        if (!m_status.ipAddress.empty()) {
            detail = U8String(m_status.ipAddress.c_str());
        }
        if (m_status.signalDbm != 0) {
            detail += (detail.empty() ? "" : "   ·   ") +
                      SignalText(m_status.signalDbm);
        }
        if (!m_status.bssid.empty()) {
            detail += (detail.empty() ? "" : "   ·   ") +
                      U8String(m_status.bssid.c_str());
        }
        m_pStatusDetail->SetText(detail);
    }
    if (m_pStatusWarning != nullptr) {
        m_pStatusWarning->SetText(m_status.haveControl
                                      ? ""
                                      : (m_status.controlDenied
                                             ? "只读：当前用户无权使用 wpa_supplicant 控制接口"
                                             : "只读：未启用 wpa_supplicant 控制接口"));
        // Say exactly what to run, rather than pointing at a document that is
        // not there.  The status is re-read every few seconds, so running the
        // command makes the window come alive on its own -- no restart, and no
        // need to know that it was waiting.
        if (m_pSetupLabel != nullptr) {
            m_pSetupLabel->SetText(m_status.haveControl ? ""
                                                         : SetupHint());
        }
    }
}

U8String WifiForm::SignalText(int dbm)
{
    return Num(dbm) + " dBm";
}

U8String WifiForm::SetupHint() const
{
    if (m_status.controlDenied) {
        // The socket is owned by a group this user is not in; joining that
        // group is the whole fix, and wpa_supplicant needs no restart for it.
        const char* user = ::getenv("USER");
        return U8String("sudo pw groupmod wheel -m ") +
               U8String(user != nullptr && *user != '\0' ? user : "用户名");
    }
    return U8String("sudo sh ") + U8String(POLLUX_WIFI_SETUP);
}

void WifiForm::SetHint(const U8String& text, bool bad)
{
    m_hint = text;
    m_hintBad = bad;
    if (m_pHintLabel != nullptr) {
        m_pHintLabel->SetText(text);
        m_pHintLabel->SetStateTextColor(ui::kControlStateNormal,
                                        bad ? m_pal.danger : m_pal.textHint);
    }
}
