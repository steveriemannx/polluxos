#ifndef EXAMPLES_POLLUXDESK_WIFI_WIFI_FORM_H_
#define EXAMPLES_POLLUXDESK_WIFI_WIFI_FORM_H_

// dui
#include "dui/dui.h"

#include "PolluxSettings.h"
#include "WifiData.h"

#include <mutex>
#include <string>
#include <thread>
#include <vector>

/** PolluxOS Wi-Fi window (pure code mode, no layout XML, Wayland/wlroots).
 *
 *  Pick a network, type its password, join it -- and the same window is where
 *  the current connection is described.  This lives in its own window rather
 *  than in the menu bar's Wi-Fi popover because the desktop shell is not given
 *  the keyboard by the compositor (it is the bottom-most full-screen surface,
 *  and focus_toplevel skips it), so a password could never be typed there.
 *
 *  The window is honest about what it can do: changing Wi-Fi goes through
 *  wpa_supplicant's control socket, which only exists when wpa_supplicant was
 *  started with one configured.  Without it the window still lists networks
 *  and reports the connection, and says in the status card why joining is
 *  unavailable.
 */
class WifiForm : public ui::WindowImplBase
{
    typedef ui::WindowImplBase BaseClass;
public:
    WifiForm();
    virtual ~WifiForm() override;

    /** Resource-related interfaces: pure code mode, no layout XML is loaded. */
    virtual DString GetSkinFolder() override;
    virtual DString GetSkinFile() override;

    /** Window creation attributes (no caption/shadow: compositor draws them). */
    virtual void GetCreateWindowAttributes(ui::WindowCreateAttributes& attrs) override;

    /** Called after the window is created, for subclasses to do some initialization work */
    virtual void OnInitWindow() override;

private:
    struct Palette
    {
        DString windowBg, cardBg, cardBorder, rowHot, rowSelected, hairline;
        DString textStrong, textBody, textHint, accent, textOnAccent, danger;
        DString fieldBg, fieldBorder, track;
    };

    // ---- construction ----------------------------------------------------
    void ApplyAppearance();
    void BuildUi();
    void BuildHeader(ui::VBox* pRoot);
    void BuildStatusCard(ui::VBox* pRoot);
    void BuildNetworkList(ui::VBox* pRoot);
    void BuildConnectBar(ui::VBox* pRoot);
    /** Fill the bar under the list for what is selected: nothing, an open
     *  network, a secured one that needs a password, or the one we are on. */
    void FillConnectBar();
    void RebuildUi();

    // ---- networking ------------------------------------------------------
    void StartScan();
    void CollectScan();
    void RebuildNetworkRows();
    void SelectNetwork(const std::string& ssid);
    void UpdateSelection();
    void UpdateConnectBar();
    void DoConnect();
    void DoDisconnect();
    void OnTick();
    void PollSettings();
    void RefreshStatus();

    // ---- helpers ---------------------------------------------------------
    ui::Label* AddLabel(ui::Box* pParent, const DString& text, const DString& font,
                        const DString& colour);
    void SetRadius(ui::Control* pControl, int radius, bool interactive);
    /** Four bars, the height of the tallest one set by the signal. */
    ui::HBox* MakeSignalBars(ui::Window* pWindow, int dbm, bool active);
    void SetHint(const DString& text, bool bad);
    static DString SignalText(int dbm);

    // ---- state -----------------------------------------------------------
    pollux::Settings m_settings;
    Palette m_pal;
    unsigned long long m_settingsMtime = 0;
    bool m_uiDirty = false;
    size_t m_timerId = 0;

    wifi::Wifi m_wifi;
    wifi::Status m_status;
    std::vector<wifi::Network> m_networks;
    std::string m_selectedSsid;

    // The scan shells out and takes a second or two, so it runs on its own
    // thread and the result is picked up by the tick: a frozen window while a
    // scan runs is exactly what this avoids.
    std::thread m_scanThread;
    std::mutex m_scanMutex;
    struct ScanOutcome
    {
        bool done = false;
        bool ok = false;
        std::vector<wifi::Network> networks;
        std::string error;
    } m_scanOutcome;
    bool m_scanRunning = false;

    // A join is watched rather than awaited: wpa_cli accepting the network
    // says nothing about the handshake, which takes a few seconds and can
    // still fail on a wrong password.
    std::string m_joiningSsid;
    int m_joinSecondsLeft = 0;
    DString m_hint;
    bool m_hintBad = false;

    // Header
    ui::Label* m_pScanButton = nullptr;
    ui::VBox* m_pStatusCard = nullptr;
    ui::Label* m_pStatusHeadline = nullptr;
    ui::Label* m_pStatusSsid = nullptr;
    ui::Label* m_pStatusDetail = nullptr;
    ui::Label* m_pStatusWarning = nullptr;

    ui::VScrollBox* m_pNetworkList = nullptr;
    ui::Label* m_pNetworkEmpty = nullptr;
    // Index-aligned with m_networks, so selecting only repaints two rows.
    std::vector<ui::ButtonHBox*> m_rowButtons;

    ui::VBox* m_pConnectBar = nullptr;      // rebuilt on selection change
    DString   m_connectBarShape;            // what it was last built for
    ui::RichEdit* m_pPassword = nullptr;
    ui::Label* m_pPasswordLabel = nullptr;
    ui::Button* m_pShowButton = nullptr;      // reveal what was typed
    ui::Button* m_pConnectButton = nullptr;
    bool m_passwordVisible = false;
    ui::Label* m_pHintLabel = nullptr;
    // The one-time command that enables the control interface, shown only
    // while it is missing.
    ui::Label* m_pSetupLabel = nullptr;
};

#endif // EXAMPLES_POLLUXDESK_WIFI_WIFI_FORM_H_
