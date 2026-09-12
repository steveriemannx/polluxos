#include "WifiForm.h"

#include "PolluxPaths.h"

#include <cstdio>
#include <cstring>

namespace {

const int kRefreshMs = 1000;
// The status is read from three programs; once a second would be three
// processes a second for a window that mostly sits still.
const int kStatusEveryTicks = 3;
// How long a join is given before it is called a failure.  A WPA handshake
// takes a couple of seconds; fifteen is already generous.
const int kJoinTimeoutSeconds = 15;

DString Num(long long value)
{
    return ui::StringUtil::Printf(DUI_T("%lld"), value);
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
WifiForm::WifiForm() = default;

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

DString WifiForm::GetSkinFolder() { return DUI_T(""); }
DString WifiForm::GetSkinFile()   { return DUI_T(""); }

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
    if (m_joinSecondsLeft > 0 || ++tick % kStatusEveryTicks == 0) {
        RefreshStatus();
    }

    // A join is watched to its end: wpa_supplicant accepting the network says
    // nothing about whether the handshake will work out.
    if (!m_joiningSsid.empty()) {
        if (m_status.connected && m_status.ssid == m_joiningSsid) {
            SetHint(DString(DUI_T("已连接到 ")) + DString(m_joiningSsid.c_str()), false);
            m_joiningSsid.clear();
            m_joinSecondsLeft = 0;
            m_pPassword->SetText(DUI_T(""));
            RebuildNetworkRows();
            UpdateConnectBar();
        } else if (--m_joinSecondsLeft <= 0) {
            DString reason = DUI_T("连接超时");
            if (!m_status.state.empty()) {
                // 4WAY_HANDSHAKE means the password was refused, which is
                // worth saying in words rather than in wpa_supplicant's.
                reason = (m_status.state == "4WAY_HANDSHAKE" ||
                          m_status.state == "ASSOCIATING")
                             ? DUI_T("连接失败：密码可能不正确")
                             : DUI_T("连接失败：") + DString(m_status.state.c_str());
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
    m_pal.accent = DString(pollux::AccentHex(m_settings));
    m_pal.textOnAccent = DUI_T("#FFFFFFFF");

    if (dark) {
        m_pal.windowBg     = DUI_T("#FF1C1C1E");
        m_pal.cardBg       = DUI_T("#FF2C2C2E");
        m_pal.cardBorder   = DUI_T("#26FFFFFF");
        m_pal.rowHot       = DUI_T("#1AFFFFFF");
        m_pal.rowSelected  = DUI_T("#330A84FF");
        m_pal.hairline     = DUI_T("#26FFFFFF");
        m_pal.textStrong   = DUI_T("#FFF5F5F7");
        m_pal.textBody     = DUI_T("#FFD8D8DC");
        m_pal.textHint     = DUI_T("#FF98989D");
        m_pal.danger       = DUI_T("#FFFF453A");
        m_pal.fieldBg      = DUI_T("#FF3A3A3C");
        m_pal.fieldBorder  = DUI_T("#33FFFFFF");
        m_pal.track        = DUI_T("#33FFFFFF");
    } else {
        m_pal.windowBg     = DUI_T("#FFF2F2F7");
        m_pal.cardBg       = DUI_T("#FFFFFFFF");
        m_pal.cardBorder   = DUI_T("#22000000");
        m_pal.rowHot       = DUI_T("#14000000");
        m_pal.rowSelected  = DUI_T("#260A84FF");
        m_pal.hairline     = DUI_T("#22000000");
        m_pal.textStrong   = DUI_T("#FF1D1D1F");
        m_pal.textBody     = DUI_T("#FF3A3A3C");
        m_pal.textHint     = DUI_T("#FF8E8E93");
        m_pal.danger       = DUI_T("#FFFF3B30");
        m_pal.fieldBg      = DUI_T("#FFFFFFFF");
        m_pal.fieldBorder  = DUI_T("#33000000");
        m_pal.track        = DUI_T("#1F000000");
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
    pControl->SetAttribute(DUI_T("border_round"),
                           ui::StringUtil::Printf(DUI_T("%d,%d"), radius, radius));
}

ui::Label* WifiForm::AddLabel(ui::Box* pParent, const DString& text,
                              const DString& font, const DString& colour)
{
    ui::Label* pLabel = new ui::Label(this);
    pLabel->SetText(text);
    pLabel->SetAttribute(DUI_T("font"), font);
    pLabel->SetStateTextColor(ui::kControlStateNormal, colour);
    pLabel->SetAttribute(DUI_T("text_align"), DUI_T("left,vcenter"));
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
    m_pConnectButton = nullptr;
    m_pHintLabel = nullptr;
    m_pSetupLabel = nullptr;

    ui::VBox* pRoot = new ui::VBox(this);
    pRoot->SetBkColor(m_pal.windowBg);
    pRoot->SetBorderColor(DUI_T("#00000000"));
    pRoot->SetAttribute(DUI_T("border_size"), DUI_T("0"));
    pRoot->SetAttribute(DUI_T("padding"), DUI_T("0,0,0,0"));

    BuildHeader(pRoot);
    BuildStatusCard(pRoot);
    BuildNetworkList(pRoot);
    BuildConnectBar(pRoot);

    AttachBox(pRoot);
}

void WifiForm::BuildHeader(ui::VBox* pRoot)
{
    ui::HBox* pHeader = new ui::HBox(this);
    pHeader->SetAttribute(DUI_T("height"), DUI_T("56"));
    pHeader->SetBkColor(m_pal.windowBg);
    pHeader->SetAttribute(DUI_T("padding"), DUI_T("16,0,16,0"));
    pHeader->SetAttribute(DUI_T("child_align"), DUI_T("vcenter"));
    pRoot->AddItem(pHeader);

    ui::Label* pTitle = AddLabel(pHeader, DUI_T("Wi-Fi"), DUI_T("system_bold_18"),
                                 m_pal.textStrong);
    pTitle->SetAttribute(DUI_T("width"), DUI_T("stretch"));

    // A scan takes a second or two; this is the button that starts one.
    m_pScanButton = new ui::Button(this);
    m_pScanButton->SetText(DUI_T("重新扫描"));
    m_pScanButton->SetAttribute(DUI_T("font"), DUI_T("system_12"));
    m_pScanButton->SetAttribute(DUI_T("width"), DUI_T("88"));
    m_pScanButton->SetAttribute(DUI_T("height"), DUI_T("28"));
    m_pScanButton->SetAttribute(DUI_T("text_align"), DUI_T("hcenter,vcenter"));
    m_pScanButton->SetAttribute(DUI_T("cursor_type"), DUI_T("hand"));
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
    m_pStatusCard->SetAttribute(DUI_T("height"), DUI_T("104"));
    m_pStatusCard->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    m_pStatusCard->SetAttribute(DUI_T("margin"), DUI_T("16,0,16,0"));
    m_pStatusCard->SetAttribute(DUI_T("padding"), DUI_T("14,10,14,10"));
    m_pStatusCard->SetBkColor(m_pal.cardBg);
    m_pStatusCard->SetBorderColor(m_pal.cardBorder);
    m_pStatusCard->SetAttribute(DUI_T("border_size"), DUI_T("1"));
    SetRadius(m_pStatusCard, 10, false);
    pRoot->AddItem(m_pStatusCard);

    m_pStatusHeadline = AddLabel(m_pStatusCard, DUI_T(""), DUI_T("system_12"),
                                 m_pal.textHint);
    m_pStatusHeadline->SetAttribute(DUI_T("height"), DUI_T("18"));
    m_pStatusHeadline->SetAttribute(DUI_T("width"), DUI_T("stretch"));

    m_pStatusSsid = AddLabel(m_pStatusCard, DUI_T(""), DUI_T("system_bold_18"),
                             m_pal.textStrong);
    m_pStatusSsid->SetAttribute(DUI_T("height"), DUI_T("28"));
    m_pStatusSsid->SetAttribute(DUI_T("width"), DUI_T("stretch"));

    m_pStatusDetail = AddLabel(m_pStatusCard, DUI_T(""), DUI_T("system_12"),
                               m_pal.textHint);
    m_pStatusDetail->SetAttribute(DUI_T("height"), DUI_T("18"));
    m_pStatusDetail->SetAttribute(DUI_T("width"), DUI_T("stretch"));

    m_pStatusWarning = AddLabel(m_pStatusCard, DUI_T(""), DUI_T("system_12"),
                                m_pal.danger);
    m_pStatusWarning->SetAttribute(DUI_T("height"), DUI_T("20"));
    m_pStatusWarning->SetAttribute(DUI_T("width"), DUI_T("stretch"));
}

void WifiForm::BuildNetworkList(ui::VBox* pRoot)
{
    ui::Label* pHeading = AddLabel(pRoot, DUI_T("网络"), DUI_T("system_12"),
                                   m_pal.textHint);
    pHeading->SetAttribute(DUI_T("height"), DUI_T("24"));
    pHeading->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    pHeading->SetAttribute(DUI_T("margin"), DUI_T("18,10,0,2"));

    m_pNetworkList = new ui::VScrollBox(this);
    m_pNetworkList->SetAttribute(DUI_T("height"), DUI_T("stretch"));
    m_pNetworkList->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    m_pNetworkList->SetAttribute(DUI_T("padding"), DUI_T("10,0,10,0"));
    m_pNetworkList->SetAttribute(DUI_T("vscrollbar"), DUI_T("true"));
    pRoot->AddItem(m_pNetworkList);

    m_pNetworkEmpty = AddLabel(m_pNetworkList, DUI_T("正在扫描…"), DUI_T("system_12"),
                               m_pal.textHint);
    m_pNetworkEmpty->SetAttribute(DUI_T("height"), DUI_T("28"));
    m_pNetworkEmpty->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    m_pNetworkEmpty->SetAttribute(DUI_T("margin"), DUI_T("8,0,0,0"));
}

void WifiForm::BuildConnectBar(ui::VBox* pRoot)
{
    ui::Control* pHairline = new ui::Control(this);
    pHairline->SetAttribute(DUI_T("height"), DUI_T("1"));
    pHairline->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    pHairline->SetBkColor(m_pal.hairline);
    pHairline->SetMouseEnabled(false);
    pRoot->AddItem(pHairline);

    // Fixed height, and its *contents* are what change: dui lays a control out
    // once, so a bar that grew and shrank would keep the size it was built
    // with.  Filling it again is safe from a click handler because the rows
    // that are clicked live in the list above, not in here.
    m_pConnectBar = new ui::VBox(this);
    m_pConnectBar->SetAttribute(DUI_T("height"), DUI_T("58"));
    m_pConnectBar->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    m_pConnectBar->SetAttribute(DUI_T("padding"), DUI_T("16,0,16,0"));
    m_pConnectBar->SetAttribute(DUI_T("child_align"), DUI_T("vcenter"));
    pRoot->AddItem(m_pConnectBar);

    m_pPassword = nullptr;
    m_pPasswordLabel = nullptr;
    m_pConnectButton = nullptr;
    m_connectBarShape.clear();

    // Two lines: what just happened, and -- in read-only mode -- the command
    // that fixes that.  The command is long enough to need a line of its own.
    ui::VBox* pHints = new ui::VBox(this);
    pHints->SetAttribute(DUI_T("height"), DUI_T("46"));
    pHints->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    pHints->SetAttribute(DUI_T("padding"), DUI_T("18,2,16,2"));
    pRoot->AddItem(pHints);

    m_pHintLabel = AddLabel(pHints, DUI_T(""), DUI_T("system_12"), m_pal.textHint);
    m_pHintLabel->SetAttribute(DUI_T("height"), DUI_T("22"));
    m_pHintLabel->SetAttribute(DUI_T("width"), DUI_T("stretch"));

    m_pSetupLabel = AddLabel(pHints, DUI_T(""), DUI_T("system_12"), m_pal.textBody);
    m_pSetupLabel->SetAttribute(DUI_T("height"), DUI_T("20"));
    m_pSetupLabel->SetAttribute(DUI_T("width"), DUI_T("stretch"));
}

void WifiForm::FillConnectBar()
{
    if (m_pConnectBar == nullptr) {
        return;
    }
    m_pConnectBar->RemoveAllItems();
    m_pPassword = nullptr;
    m_pPasswordLabel = nullptr;
    m_pConnectButton = nullptr;

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
    pRow->SetAttribute(DUI_T("height"), DUI_T("34"));
    pRow->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    pRow->SetAttribute(DUI_T("child_align"), DUI_T("vcenter"));
    m_pConnectBar->AddItem(pRow);

    if (!m_status.haveControl) {
        ui::Label* pHint = AddLabel(pRow, DUI_T("只读模式：控制接口未启用，见下方命令"),
                                    DUI_T("system_12"), m_pal.textHint);
        pHint->SetAttribute(DUI_T("width"), DUI_T("stretch"));
        m_connectBarShape = DUI_T("readonly");
        return;
    }
    if (network == nullptr) {
        ui::Label* pHint = AddLabel(pRow, DUI_T("点击上面的网络，输入密码后连接"),
                                    DUI_T("system_12"), m_pal.textHint);
        pHint->SetAttribute(DUI_T("width"), DUI_T("stretch"));
        m_connectBarShape = DUI_T("none");
        return;
    }

    const bool needPassword = network->secured && !connectedHere;

    if (needPassword) {
        m_pPasswordLabel = AddLabel(pRow, DUI_T("密码"), DUI_T("system_14"),
                                    m_pal.textBody);
        m_pPasswordLabel->SetAttribute(DUI_T("width"), DUI_T("44"));

        // The password goes into a real text field, which is the whole reason
        // this window exists: the desktop shell never receives the keyboard.
        m_pPassword = new ui::RichEdit(this);
        m_pPassword->SetAttribute(DUI_T("width"), DUI_T("stretch"));
        m_pPassword->SetAttribute(DUI_T("height"), DUI_T("32"));
        m_pPassword->SetAttribute(DUI_T("margin"), DUI_T("0,0,10,0"));
        m_pPassword->SetFontId(DUI_T("system_14"));
        m_pPassword->SetTextColor(m_pal.textStrong);
        m_pPassword->SetBkColor(m_pal.fieldBg);
        m_pPassword->SetBorderColor(m_pal.fieldBorder);
        m_pPassword->SetAttribute(DUI_T("border_size"), DUI_T("1"));
        m_pPassword->SetTextPadding(ui::UiPadding(10, 0, 10, 0), false);
        m_pPassword->SetPasswordMode(true);
        m_pPassword->SetLimitText(64);
        SetRadius(m_pPassword, 8, false);
        m_pPassword->AttachReturn([this](const ui::EventArgs& /*args*/) {
            DoConnect();
            return true;
        });
        pRow->AddItem(m_pPassword);
    } else {
        ui::Label* pNote = AddLabel(pRow,
                                    connectedHere
                                        ? DUI_T("这是当前连接的网络")
                                        : DUI_T("开放网络，无需密码"),
                                    DUI_T("system_12"), m_pal.textHint);
        pNote->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    }

    m_pConnectButton = new ui::Button(this);
    m_pConnectButton->SetText(connectedHere ? DUI_T("断开") : DUI_T("连接"));
    m_pConnectButton->SetAttribute(DUI_T("font"), DUI_T("system_14"));
    m_pConnectButton->SetAttribute(DUI_T("width"), DUI_T("88"));
    m_pConnectButton->SetAttribute(DUI_T("height"), DUI_T("32"));
    m_pConnectButton->SetAttribute(DUI_T("text_align"), DUI_T("hcenter,vcenter"));
    m_pConnectButton->SetAttribute(DUI_T("cursor_type"), DUI_T("hand"));
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

    m_connectBarShape = connectedHere ? DUI_T("connected")
                                      : (needPassword ? DUI_T("secure") : DUI_T("open"));

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
        m_pNetworkEmpty->SetText(DUI_T("正在扫描…"));
        m_pNetworkEmpty->SetAttribute(DUI_T("height"),
                                      m_networks.empty() ? DUI_T("28") : DUI_T("0"));
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
    } else {
        m_networks.clear();
    }

    RebuildNetworkRows();
    if (!outcome.ok) {
        SetHint(DString(outcome.error.c_str()), true);
    } else if (m_pNetworkEmpty != nullptr) {
        m_pNetworkEmpty->SetText(m_networks.empty() ? DUI_T("没有扫描到网络") : DUI_T(""));
        m_pNetworkEmpty->SetAttribute(DUI_T("height"),
                                      m_networks.empty() ? DUI_T("28") : DUI_T("0"));
    }
    UpdateConnectBar();
}

ui::HBox* WifiForm::MakeSignalBars(ui::Window* pWindow, int dbm, bool active)
{
    // Four bars, the number of them lit telling the strength at a glance.
    const int level = SignalLevel(dbm);
    ui::HBox* pBars = new ui::HBox(pWindow);
    pBars->SetAttribute(DUI_T("width"), DUI_T("26"));
    pBars->SetAttribute(DUI_T("height"), DUI_T("16"));
    pBars->SetAttribute(DUI_T("margin"), DUI_T("0,0,10,0"));
    pBars->SetAttribute(DUI_T("child_align"), DUI_T("vbottom"));
    pBars->SetMouseEnabled(false);

    const int heights[] = { 4, 7, 10, 13 };
    for (int i = 0; i < 4; ++i) {
        ui::Control* pBar = new ui::Control(pWindow);
        pBar->SetAttribute(DUI_T("width"), DUI_T("4"));
        pBar->SetAttribute(DUI_T("height"), Num(heights[i]));
        pBar->SetAttribute(DUI_T("margin"), DUI_T("0,0,2,0"));
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

    m_pNetworkEmpty = AddLabel(m_pNetworkList, DUI_T(""), DUI_T("system_12"),
                               m_pal.textHint);
    m_pNetworkEmpty->SetAttribute(DUI_T("height"),
                                  m_networks.empty() ? DUI_T("28") : DUI_T("0"));
    m_pNetworkEmpty->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    m_pNetworkEmpty->SetAttribute(DUI_T("margin"), DUI_T("8,0,0,0"));

    for (const wifi::Network& network : m_networks) {
        const std::string ssid = network.ssid;
        const bool connected = network.connected ||
                               (!m_status.ssid.empty() && ssid == m_status.ssid);

        ui::ButtonHBox* pRow = new ui::ButtonHBox(this);
        pRow->SetAttribute(DUI_T("height"), DUI_T("38"));
        pRow->SetAttribute(DUI_T("width"), DUI_T("stretch"));
        pRow->SetAttribute(DUI_T("padding"), DUI_T("8,0,8,0"));
        pRow->SetAttribute(DUI_T("cursor_type"), DUI_T("hand"));
        pRow->SetAttribute(DUI_T("child_align"), DUI_T("vcenter"));
        pRow->SetStateColor(ui::kControlStateNormal, DUI_T("#00000000"));
        pRow->SetStateColor(ui::kControlStateHot, m_pal.rowHot);
        SetRadius(pRow, 8, true);
        pRow->AttachClick([this, ssid](const ui::EventArgs& /*args*/) {
            SelectNetwork(ssid);
            return true;
        });
        m_pNetworkList->AddItem(pRow);
        m_rowButtons.push_back(pRow);

        pRow->AddItem(MakeSignalBars(this, network.signalDbm, network.saved));

        ui::Label* pName = AddLabel(pRow, DString(ssid.c_str()), DUI_T("system_14"),
                                    m_pal.textStrong);
        pName->SetAttribute(DUI_T("width"), DUI_T("stretch"));

        ui::Label* pSecurity = AddLabel(pRow, DString(network.security.c_str()),
                                        DUI_T("system_12"), m_pal.textHint);
        pSecurity->SetAttribute(DUI_T("width"), DUI_T("64"));
        pSecurity->SetAttribute(DUI_T("text_align"), DUI_T("right,vcenter"));

        ui::Label* pState = AddLabel(pRow,
                                     connected ? DUI_T("已连接")
                                               : (network.saved ? DUI_T("已保存")
                                                                : DUI_T("")),
                                     DUI_T("system_12"),
                                     connected ? m_pal.accent : m_pal.textHint);
        pState->SetAttribute(DUI_T("width"), DUI_T("60"));
        pState->SetAttribute(DUI_T("text_align"), DUI_T("right,vcenter"));
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
    if (network != nullptr && network->secured && !network->saved &&
        m_status.ssid != ssid) {
        // A network we have no key for: the field is what is wanted next.
        m_pPassword->SetText(DUI_T(""));
        m_pPassword->SetFocus();
        SetHint(DUI_T("输入 Wi-Fi 密码后点连接（也可以直接按回车）"), false);
    } else if (network != nullptr && network->saved && m_status.ssid != ssid) {
        SetHint(DUI_T("已保存的网络，直接点连接即可"), false);
    }
}

void WifiForm::UpdateSelection()
{
    for (size_t i = 0; i < m_rowButtons.size() && i < m_networks.size(); ++i) {
        const bool selected = m_networks[i].ssid == m_selectedSsid;
        m_rowButtons[i]->SetStateColor(ui::kControlStateNormal,
                                       selected ? m_pal.rowSelected
                                                : DUI_T("#00000000"));
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

    DString shape;
    if (!m_status.haveControl) {
        shape = DUI_T("readonly");
    } else if (network == nullptr) {
        shape = DUI_T("none");
    } else if (connectedHere) {
        shape = DUI_T("connected");
    } else {
        shape = network->secured ? DUI_T("secure") : DUI_T("open");
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
        SetHint(DUI_T("先在上面选一个网络"), true);
        return;
    }
    if (!m_status.haveControl) {
        SetHint(DUI_T("wpa_supplicant 控制接口不可用，见上方提示"), true);
        return;
    }
    if (!m_status.ssid.empty() && network->ssid == m_status.ssid) {
        DoDisconnect();
        return;
    }

    DString typed = m_pPassword != nullptr ? m_pPassword->GetText() : DString();
    std::string psk(typed.c_str());
    if (network->secured && !network->saved && psk.empty()) {
        SetHint(DUI_T("这个网络需要密码"), true);
        m_pPassword->SetFocus();
        return;
    }

    std::string error;
    if (!m_wifi.Connect(network->ssid, psk, network->secured, error)) {
        SetHint(DString((std::string("无法连接：") + error).c_str()), true);
        return;
    }

    m_joiningSsid = network->ssid;
    m_joinSecondsLeft = kJoinTimeoutSeconds;
    SetHint(DString((std::string("正在连接 ") + network->ssid + " …").c_str()), false);
}

void WifiForm::DoDisconnect()
{
    std::string error;
    if (!m_wifi.Disconnect(error)) {
        SetHint(DString((std::string("无法断开：") + error).c_str()), true);
        return;
    }
    SetHint(DUI_T("已断开。选一个网络可以重新连接。"), false);
    RefreshStatus();
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

    if (m_pStatusHeadline != nullptr) {
        if (m_wifi.Interface().empty()) {
            m_pStatusHeadline->SetText(DUI_T("没有无线网卡"));
            m_pStatusHeadline->SetStateTextColor(ui::kControlStateNormal, m_pal.danger);
        } else if (m_status.connected) {
            m_pStatusHeadline->SetText(DUI_T("已连接"));
            m_pStatusHeadline->SetStateTextColor(ui::kControlStateNormal, m_pal.accent);
        } else if (!m_joiningSsid.empty()) {
            m_pStatusHeadline->SetText(DUI_T("正在连接…"));
            m_pStatusHeadline->SetStateTextColor(ui::kControlStateNormal, m_pal.textBody);
        } else {
            m_pStatusHeadline->SetText(DUI_T("未连接"));
            m_pStatusHeadline->SetStateTextColor(ui::kControlStateNormal, m_pal.textHint);
        }
    }
    if (m_pStatusSsid != nullptr) {
        m_pStatusSsid->SetText(m_status.ssid.empty() ? DUI_T("—")
                                                     : DString(m_status.ssid.c_str()));
    }
    if (m_pStatusDetail != nullptr) {
        DString detail;
        if (!m_status.ipAddress.empty()) {
            detail = DString(m_status.ipAddress.c_str());
        }
        if (m_status.signalDbm != 0) {
            detail += (detail.empty() ? DUI_T("") : DUI_T("   ·   ")) +
                      SignalText(m_status.signalDbm);
        }
        if (!m_status.bssid.empty()) {
            detail += (detail.empty() ? DUI_T("") : DUI_T("   ·   ")) +
                      DString(m_status.bssid.c_str());
        }
        m_pStatusDetail->SetText(detail);
    }
    if (m_pStatusWarning != nullptr) {
        m_pStatusWarning->SetText(m_status.haveControl
                                      ? DUI_T("")
                                      : DUI_T("只读：未启用 wpa_supplicant 控制接口"));
        // Say exactly what to run, rather than pointing at a document that is
        // not there.  The status is re-read every few seconds, so running the
        // script makes the window come alive on its own -- no restart, and no
        // need to know that it was waiting.
        if (m_pSetupLabel != nullptr) {
            m_pSetupLabel->SetText(m_status.haveControl
                                       ? DUI_T("")
                                       : DUI_T("sudo sh " POLLUX_WIFI_SETUP));
        }
    }
}

DString WifiForm::SignalText(int dbm)
{
    return Num(dbm) + DUI_T(" dBm");
}

void WifiForm::SetHint(const DString& text, bool bad)
{
    m_hint = text;
    m_hintBad = bad;
    if (m_pHintLabel != nullptr) {
        m_pHintLabel->SetText(text);
        m_pHintLabel->SetStateTextColor(ui::kControlStateNormal,
                                        bad ? m_pal.danger : m_pal.textHint);
    }
}
