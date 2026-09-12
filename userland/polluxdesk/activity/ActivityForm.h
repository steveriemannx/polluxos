#ifndef EXAMPLES_POLLUXDESK_ACTIVITY_ACTIVITY_FORM_H_
#define EXAMPLES_POLLUXDESK_ACTIVITY_ACTIVITY_FORM_H_

// dui
#include "dui/dui.h"

#include "ActivityData.h"
#include "ActivityWidgets.h"
#include "PolluxSettings.h"

#include <string>
#include <vector>

/** PolluxOS activity monitor (pure code mode, no layout XML, Wayland/wlroots).
 *
 *  The desktop's own Activity Monitor: what the machine is doing, read from
 *  the kernel every second.  Four tabs -- CPU, memory, disks, network -- over a
 *  process table that stays visible whichever one is open, which is how the
 *  macOS window of the same name is arranged.
 *
 *  Two things about how it is built are worth knowing before reading the code:
 *
 *    - The charts and bars are custom-painted controls (see ActivityWidgets.h)
 *      because dui lays a control out once.  A bar whose height has to follow
 *      a reading cannot be an ordinary box.
 *    - Anything whose *size* depends on a reading is built at its final size
 *      and only has its text or colours changed afterwards: the process table
 *      is a fixed set of rows that get rewritten, and switching tabs rebuilds
 *      the panel (which is safe: the buttons that switch it live outside it).
 */
class ActivityForm : public ui::WindowImplBase
{
    typedef ui::WindowImplBase BaseClass;
public:
    ActivityForm();
    virtual ~ActivityForm() override;

    /** Resource-related interfaces: pure code mode, no layout XML is loaded. */
    virtual DString GetSkinFolder() override;
    virtual DString GetSkinFile() override;

    /** Window creation attributes (no caption/shadow: compositor draws them). */
    virtual void GetCreateWindowAttributes(ui::WindowCreateAttributes& attrs) override;

    /** Called after the window is created, for subclasses to do some initialization work */
    virtual void OnInitWindow() override;

private:
    enum PanelIndex
    {
        kPanelCpu = 0,
        kPanelMemory,
        kPanelDisk,
        kPanelNetwork,
        kPanelCount,
    };

    enum ProcessColumn
    {
        kColName = 0,
        kColPid,
        kColCpu,
        kColTime,
        kColMemory,
        kColThreads,
        kColState,
        kColCount,
    };

    /** Every colour the window uses, in both appearances.  Rebuilt from the
     *  desktop's settings before each build, so the window follows the
     *  appearance and the accent colour chosen in the settings app. */
    struct Palette
    {
        DString windowBg, cardBg, cardBorder, headerBg, rowAlternate, hairline;
        DString textStrong, textBody, textHint, accent, textOnAccent;
        DString track, grid, tabTrack;
        DString cpuUser, cpuSystem, memUsed, diskFill, diskFull, netDown, netUp;
    };

    // ---- construction ----------------------------------------------------
    void ApplyAppearance();
    void BuildUi();
    void BuildHeader(ui::VBox* pRoot);
    void BuildProcessCard(ui::VBox* pRoot);
    void BuildStatusBar(ui::VBox* pRoot);
    void RebuildUi();

    /** Fill the panel host for one tab.  Called from a tab button, which is
     *  safe because those buttons live in the header, not in the panel being
     *  emptied. */
    void ShowPanel(int index);
    void BuildCpuPanel(ui::VBox* pPanel);
    void BuildMemoryPanel(ui::VBox* pPanel);
    void BuildDiskPanel(ui::VBox* pPanel);
    void BuildNetworkPanel(ui::VBox* pPanel);
    void UpdateTabStyles();

    // ---- once a second ---------------------------------------------------
    void StartTimer();
    void OnTick();
    void PollSettings();
    void SampleAll();
    void RefreshPanel();
    void RefreshCpuPanel();
    void RefreshMemoryPanel();
    void RefreshDiskPanel();
    void RefreshNetworkPanel();
    void RefreshProcessTable();
    void RefreshStatusBar();

    // ---- process table ---------------------------------------------------
    void SortProcesses();
    void SetSortColumn(int column);
    void UpdateColumnTitles();
    void FillProcessRow(size_t row, const activity::ProcessInfo& info);

    // ---- helpers ---------------------------------------------------------
    ui::Label* AddLabel(ui::Box* pParent, const DString& text, const DString& font,
                        const DString& colour);
    ui::VBox* AddCard(ui::Box* pParent, const DString& height);
    /** A legend chip: a coloured dot, a name and its reading.  The label is
     *  returned because the reading is what changes every second. */
    ui::Label* AddLegend(ui::Box* pParent, const DString& colour,
                         const DString& text, const DString& width);
    void SetRadius(ui::Control* pControl, int radius, bool interactive);

    static DString FormatBytes(unsigned long long bytes);
    static DString FormatRate(double bytesPerSecond);
    static DString FormatDuration(double seconds);
    static DString FormatUptime(long seconds);

    // ---- state -----------------------------------------------------------
    pollux::Settings m_settings;
    Palette m_pal;
    unsigned long long m_settingsMtime = 0;
    bool m_uiDirty = false;
    size_t m_timerId = 0;

    activity::Sampler m_sampler;
    activity::CpuReading m_cpu;
    activity::MemReading m_mem;
    std::vector<activity::ProcessInfo> m_processes;
    std::vector<activity::DiskInfo> m_disks;
    std::vector<activity::NetInfo> m_network;
    // Readings that need a scale of their own, charted over the last two
    // minutes.  Memory is charted in bytes, the network in bytes per second.
    std::vector<float> m_cpuHistory, m_memHistory, m_netDownHistory, m_netUpHistory;

    int  m_panel = kPanelCpu;
    int  m_sortColumn = kColCpu;
    bool m_sortAscending = false;
    std::string m_hostName;
    DString m_lastSortTitle;

    // Header
    ui::HBox*   m_pHeader = nullptr;
    ui::HBox*   m_pTabs = nullptr;
    ui::Button* m_pTabButtons[kPanelCount] = { nullptr, nullptr, nullptr, nullptr };
    ui::Label*  m_pHostLabel = nullptr;
    ui::VBox*   m_pPanelHost = nullptr;

    // CPU panel
    activity::HistoryGraph* m_pCpuGraph = nullptr;
    ui::Label*  m_pCpuLoad = nullptr;
    std::vector<ui::Label*> m_pCpuLegend;      // system / user / idle / temperature
    std::vector<activity::MeterBar*> m_pCoreBars;
    std::vector<ui::Label*> m_pCoreReadings;

    // Memory panel
    activity::HistoryGraph* m_pMemGraph = nullptr;
    ui::Label*  m_pMemHeadline = nullptr;
    std::vector<ui::Label*> m_pMemValues;      // active / wired / inactive / cache / free

    // Disk panel
    struct DiskRow
    {
        ui::Label* name = nullptr;
        ui::Label* detail = nullptr;
        activity::MeterBar* bar = nullptr;
    };
    std::vector<DiskRow> m_diskRows;
    ui::Label* m_pDiskEmpty = nullptr;

    // Network panel: two charts side by side, the way the macOS window has
    // them, rather than one chart with two overlapping series.
    activity::HistoryGraph* m_pNetDown = nullptr;
    activity::HistoryGraph* m_pNetUp = nullptr;
    ui::Label* m_pNetHeadline = nullptr;
    struct NetRow
    {
        ui::Label* name = nullptr;
        ui::Label* rate = nullptr;
        ui::Label* detail = nullptr;
    };
    std::vector<NetRow> m_netRows;
    ui::Label* m_pNetEmpty = nullptr;

    // Process table
    struct ProcessRow
    {
        ui::HBox*  box = nullptr;
        ui::Label* cells[kColCount] = { nullptr, nullptr, nullptr, nullptr,
                                        nullptr, nullptr, nullptr };
    };
    std::vector<ProcessRow> m_processRows;
    ui::Button* m_pColumnButtons[kColCount] = { nullptr, nullptr, nullptr,
                                                nullptr, nullptr, nullptr,
                                                nullptr };
    ui::Label* m_pProcessEmpty = nullptr;

    // Status bar
    ui::Label* m_pStatusLeft = nullptr;
    ui::Label* m_pStatusRight = nullptr;
};

#endif // EXAMPLES_POLLUXDESK_ACTIVITY_ACTIVITY_FORM_H_
