#include "ActivityForm.h"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace {

const int kRefreshMs     = 1000;
const int kProcessRows   = 12;
const int kCoreColumns   = 2;
// Two minutes at one sample a second.  ActivityWidgets keeps the same number
// of slots, so the chart is always the whole history it was given.
const int kHistoryLength = 120;
const int kMaxDiskRows   = 4;
const int kMaxNetRows    = 4;

DString Num(long long value)
{
    return ui::StringUtil::Printf(DUI_T("%lld"), value);
}

DString FormatDouble(double value, const char* format)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), format, value);
    return DString(buffer);
}

// "#AARRGGBB" with a different alpha: the fill under a graph line is the line's
// own colour, faded.
DString WithAlpha(const DString& colour, const char* alpha)
{
    if (colour.size() != 9 || colour[0] != DUI_T('#')) {
        return colour;
    }
    return DString(DUI_T("#")) + DString(alpha) + colour.substr(3);
}

const char* kPanelTitles[] = { "CPU", "内存", "磁盘", "网络" };

struct ColumnDef
{
    const char* title;
    const char* width;
    const char* align;
};

// Every column but the name has a fixed width, so the header row and the
// process rows line up without either of them measuring text.
const ColumnDef kColumnDefs[] = {
    { "进程名称", "stretch", "left,vcenter" },
    { "PID",      "72",      "right,vcenter" },
    { "% CPU",    "78",      "right,vcenter" },
    { "CPU 时间", "92",      "right,vcenter" },
    { "内存",     "90",      "right,vcenter" },
    { "线程",     "58",      "right,vcenter" },
    { "状态",     "70",      "right,vcenter" },
};

}  // namespace

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------
ActivityForm::ActivityForm() = default;

ActivityForm::~ActivityForm()
{
    if (m_timerId > 0) {
        ui::GlobalManager::Instance().Timer().RemoveTimer(m_timerId);
        m_timerId = 0;
    }
}

DString ActivityForm::GetSkinFolder() { return DUI_T(""); }
DString ActivityForm::GetSkinFile()   { return DUI_T(""); }

void ActivityForm::GetCreateWindowAttributes(ui::WindowCreateAttributes& attrs)
{
    attrs.m_bInitSizeDefined = true;
    attrs.m_szInitSize.cx = 980;
    attrs.m_szInitSize.cy = 700;
    attrs.m_bShadowAttached = false;
    attrs.m_bShadowAttachedDefined = true;
    attrs.m_bIsLayeredWindow = true;
    attrs.m_bIsLayeredWindowDefined = true;
    // No caption and no resize border: the wlroots compositor draws the title
    // bar and the traffic lights above this surface.
    attrs.m_rcCaption = ui::UiRect(0, 0, 0, 0);
    attrs.m_bCaptionDefined = true;
    attrs.m_rcSizeBox = ui::UiRect(0, 0, 0, 0);
    attrs.m_bSizeBoxDefined = true;
    BaseClass::GetCreateWindowAttributes(attrs);
}

void ActivityForm::OnInitWindow()
{
    SetShadowAttached(false);

    m_settings = pollux::Load();
    m_settingsMtime = pollux::MtimeNs(pollux::ConfigPath());
    ApplyAppearance();

    char host[128] = { 0 };
    if (gethostname(host, sizeof(host) - 1) == 0) {
        m_hostName = host;
    }

    // Read once before building: the CPU panel is laid out for the number of
    // cores the machine actually has, and a panel cannot be resized afterwards.
    SampleAll();

    BuildUi();
    BaseClass::OnInitWindow();

    RefreshPanel();
    RefreshProcessTable();
    RefreshStatusBar();
    StartTimer();
}

void ActivityForm::StartTimer()
{
    // A large positive repeat count, not dui's documented -1: the timer only
    // re-arms while uRepeatTime > 0, so -1 runs the callback once and stops.
    constexpr int32_t kRepeatForSession = 0x7FFFFFFF;
    m_timerId = ui::GlobalManager::Instance().Timer().AddTimer(GetWeakFlag(), [this]() {
        OnTick();
    }, kRefreshMs, kRepeatForSession);
}

void ActivityForm::OnTick()
{
    PollSettings();
    if (m_uiDirty) {
        // Between ticks, never inside an event handler: AttachBox deletes the
        // previous control tree synchronously.
        m_uiDirty = false;
        RebuildUi();
    }

    SampleAll();
    RefreshPanel();
    RefreshProcessTable();
    RefreshStatusBar();
}

void ActivityForm::PollSettings()
{
    const unsigned long long mtime = pollux::MtimeNs(pollux::ConfigPath());
    if (mtime == m_settingsMtime) {
        return;
    }
    m_settingsMtime = mtime;
    m_settings = pollux::Load();
    m_uiDirty = true;
}

void ActivityForm::RebuildUi()
{
    ApplyAppearance();
    BuildUi();
    RefreshPanel();
    RefreshProcessTable();
    RefreshStatusBar();
    // InvalidateAll, not Invalidate: inside a window that call is the
    // WindowBase one, which wants a rectangle.
    InvalidateAll();
}

// ---------------------------------------------------------------------------
// Appearance
// ---------------------------------------------------------------------------
void ActivityForm::ApplyAppearance()
{
    const bool dark = pollux::IsDark(m_settings);
    m_pal.accent = DString(pollux::AccentHex(m_settings));
    m_pal.textOnAccent = DUI_T("#FFFFFFFF");

    if (dark) {
        m_pal.windowBg      = DUI_T("#FF1C1C1E");
        m_pal.cardBg        = DUI_T("#FF2C2C2E");
        m_pal.cardBorder    = DUI_T("#26FFFFFF");
        m_pal.headerBg      = DUI_T("#FF242426");
        m_pal.rowAlternate  = DUI_T("#0DFFFFFF");
        m_pal.hairline      = DUI_T("#26FFFFFF");
        m_pal.textStrong    = DUI_T("#FFF5F5F7");
        m_pal.textBody      = DUI_T("#FFD8D8DC");
        m_pal.textHint      = DUI_T("#FF98989D");
        m_pal.track         = DUI_T("#33FFFFFF");
        m_pal.grid          = DUI_T("#1FFFFFFF");
        m_pal.tabTrack      = DUI_T("#1FFFFFFF");
        m_pal.cpuUser       = DUI_T("#FF0A84FF");
        m_pal.cpuSystem     = DUI_T("#FFFF453A");
        m_pal.memUsed       = DUI_T("#FF32D74B");
        m_pal.diskFill      = DUI_T("#FF0A84FF");
        m_pal.diskFull      = DUI_T("#FFFF453A");
        m_pal.netDown       = DUI_T("#FF32D74B");
        m_pal.netUp         = DUI_T("#FFFF9F0A");
    } else {
        m_pal.windowBg      = DUI_T("#FFF2F2F7");
        m_pal.cardBg        = DUI_T("#FFFFFFFF");
        m_pal.cardBorder    = DUI_T("#22000000");
        m_pal.headerBg      = DUI_T("#FFF7F7F9");
        m_pal.rowAlternate  = DUI_T("#0A000000");
        m_pal.hairline      = DUI_T("#22000000");
        m_pal.textStrong    = DUI_T("#FF1D1D1F");
        m_pal.textBody      = DUI_T("#FF3A3A3C");
        m_pal.textHint      = DUI_T("#FF8E8E93");
        m_pal.track         = DUI_T("#1F000000");
        m_pal.grid          = DUI_T("#14000000");
        m_pal.tabTrack      = DUI_T("#14000000");
        m_pal.cpuUser       = DUI_T("#FF007AFF");
        m_pal.cpuSystem     = DUI_T("#FFFF3B30");
        m_pal.memUsed       = DUI_T("#FF34C759");
        m_pal.diskFill      = DUI_T("#FF007AFF");
        m_pal.diskFull      = DUI_T("#FFFF3B30");
        m_pal.netDown       = DUI_T("#FF34C759");
        m_pal.netUp         = DUI_T("#FFFF9500");
    }
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------
void ActivityForm::SetRadius(ui::Control* pControl, int radius, bool interactive)
{
    // dui takes a corner radius through two independent APIs; giving them
    // different numbers silently draws two different corners.
    const ui::UiSize size(radius, radius);
    pControl->SetStateColorRound(ui::kControlStateNormal, size, false);
    if (interactive) {
        pControl->SetStateColorRound(ui::kControlStateHot, size, false);
        pControl->SetStateColorRound(ui::kControlStatePushed, size, false);
    }
    pControl->SetAttribute(DUI_T("border_round"),
                           ui::StringUtil::Printf(DUI_T("%d,%d"), radius, radius));
}

ui::Label* ActivityForm::AddLabel(ui::Box* pParent, const DString& text,
                                  const DString& font, const DString& colour)
{
    ui::Label* pLabel = new ui::Label(this);
    pLabel->SetText(text);
    pLabel->SetAttribute(DUI_T("font"), font);
    // SetStateTextColor rather than the "text_color" attribute: that attribute
    // does not exist for a label, and dui falls back to a default colour, which
    // is how a window ends up unreadable the moment the desktop goes dark.
    pLabel->SetStateTextColor(ui::kControlStateNormal, colour);
    pLabel->SetAttribute(DUI_T("text_align"), DUI_T("left,vcenter"));
    pLabel->SetMouseEnabled(false);
    pParent->AddItem(pLabel);
    return pLabel;
}

ui::VBox* ActivityForm::AddCard(ui::Box* pParent, const DString& height)
{
    ui::VBox* pCard = new ui::VBox(this);
    pCard->SetAttribute(DUI_T("height"), height);
    pCard->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    pCard->SetBkColor(m_pal.cardBg);
    pCard->SetBorderColor(m_pal.cardBorder);
    pCard->SetAttribute(DUI_T("border_size"), DUI_T("1"));
    pCard->SetAttribute(DUI_T("padding"), DUI_T("8,6,8,6"));
    SetRadius(pCard, 10, false);
    pParent->AddItem(pCard);
    return pCard;
}

ui::Label* ActivityForm::AddLegend(ui::Box* pParent, const DString& colour,
                                   const DString& text, const DString& width)
{
    ui::HBox* pItem = new ui::HBox(this);
    pItem->SetAttribute(DUI_T("width"), width);
    pItem->SetAttribute(DUI_T("height"), DUI_T("22"));
    pItem->SetAttribute(DUI_T("child_align"), DUI_T("vcenter"));

    ui::Control* pDot = new ui::Control(this);
    pDot->SetAttribute(DUI_T("width"), DUI_T("8"));
    pDot->SetAttribute(DUI_T("height"), DUI_T("8"));
    pDot->SetAttribute(DUI_T("margin"), DUI_T("0,0,6,0"));
    pDot->SetBkColor(colour);
    SetRadius(pDot, 4, false);
    pDot->SetMouseEnabled(false);
    pItem->AddItem(pDot);

    ui::Label* pLabel = AddLabel(pItem, text, DUI_T("system_12"), m_pal.textBody);
    pLabel->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    pParent->AddItem(pItem);
    return pLabel;
}

void ActivityForm::BuildUi()
{
    // A rebuild throws the whole tree away: forget every pointer into it first.
    m_pHeader = nullptr;
    m_pTabs = nullptr;
    m_pHostLabel = nullptr;
    m_pPanelHost = nullptr;
    m_pCpuGraph = nullptr;
    m_pCpuLoad = nullptr;
    m_pMemGraph = nullptr;
    m_pMemHeadline = nullptr;
    m_pNetDown = nullptr;
    m_pNetUp = nullptr;
    m_pNetHeadline = nullptr;
    m_pDiskEmpty = nullptr;
    m_pNetEmpty = nullptr;
    m_pProcessEmpty = nullptr;
    m_pStatusLeft = nullptr;
    m_pStatusRight = nullptr;
    m_pCpuLegend.clear();
    m_pCoreBars.clear();
    m_pCoreReadings.clear();
    m_pMemValues.clear();
    m_diskRows.clear();
    m_netRows.clear();
    m_processRows.clear();
    for (int i = 0; i < kPanelCount; ++i) {
        m_pTabButtons[i] = nullptr;
    }
    for (int i = 0; i < kColCount; ++i) {
        m_pColumnButtons[i] = nullptr;
    }

    ui::VBox* pRoot = new ui::VBox(this);
    pRoot->SetBkColor(m_pal.windowBg);
    pRoot->SetBorderColor(DUI_T("#00000000"));
    pRoot->SetAttribute(DUI_T("border_size"), DUI_T("0"));
    pRoot->SetAttribute(DUI_T("padding"), DUI_T("0,0,0,0"));

    BuildHeader(pRoot);

    ui::Control* pHairline = new ui::Control(this);
    pHairline->SetAttribute(DUI_T("height"), DUI_T("1"));
    pHairline->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    pHairline->SetBkColor(m_pal.hairline);
    pHairline->SetMouseEnabled(false);
    pRoot->AddItem(pHairline);

    ui::VBox* pBody = new ui::VBox(this);
    pBody->SetAttribute(DUI_T("height"), DUI_T("stretch"));
    pBody->SetAttribute(DUI_T("padding"), DUI_T("16,10,16,6"));
    pRoot->AddItem(pBody);

    m_pPanelHost = new ui::VBox(this);
    m_pPanelHost->SetAttribute(DUI_T("height"), DUI_T("stretch"));
    m_pPanelHost->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    pBody->AddItem(m_pPanelHost);

    BuildProcessCard(pBody);
    BuildStatusBar(pRoot);

    AttachBox(pRoot);

    ShowPanel(m_panel);
    UpdateTabStyles();
}

void ActivityForm::BuildHeader(ui::VBox* pRoot)
{
    m_pHeader = new ui::HBox(this);
    m_pHeader->SetAttribute(DUI_T("height"), DUI_T("56"));
    m_pHeader->SetBkColor(m_pal.windowBg);
    m_pHeader->SetAttribute(DUI_T("padding"), DUI_T("16,0,16,0"));
    m_pHeader->SetAttribute(DUI_T("child_align"), DUI_T("vcenter"));
    pRoot->AddItem(m_pHeader);

    // A segmented control, built out of buttons: dui's own Combo opens a second
    // window, and this backend routes input to one window per process.
    m_pTabs = new ui::HBox(this);
    m_pTabs->SetAttribute(DUI_T("width"), DUI_T("auto"));
    m_pTabs->SetAttribute(DUI_T("height"), DUI_T("32"));
    m_pTabs->SetBkColor(m_pal.tabTrack);
    m_pTabs->SetAttribute(DUI_T("padding"), DUI_T("2,2,2,2"));
    m_pTabs->SetAttribute(DUI_T("child_align"), DUI_T("vcenter"));
    SetRadius(m_pTabs, 8, false);
    m_pHeader->AddItem(m_pTabs);

    for (int i = 0; i < kPanelCount; ++i) {
        ui::Button* pTab = new ui::Button(this);
        pTab->SetText(DString(kPanelTitles[i]));
        pTab->SetAttribute(DUI_T("font"), DUI_T("system_14"));
        pTab->SetAttribute(DUI_T("width"), DUI_T("78"));
        pTab->SetAttribute(DUI_T("height"), DUI_T("26"));
        pTab->SetAttribute(DUI_T("margin"), DUI_T("2,0,2,0"));
        pTab->SetAttribute(DUI_T("text_align"), DUI_T("hcenter,vcenter"));
        pTab->SetAttribute(DUI_T("cursor_type"), DUI_T("hand"));
        SetRadius(pTab, 6, true);
        pTab->AttachClick([this, i](const ui::EventArgs& /*args*/) {
            if (m_panel != i) {
                m_panel = i;
                ShowPanel(i);
                UpdateTabStyles();
            }
            return true;
        });
        m_pTabs->AddItem(pTab);
        m_pTabButtons[i] = pTab;
    }

    ui::Control* pSpacer = new ui::Control(this);
    pSpacer->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    pSpacer->SetMouseEnabled(false);
    m_pHeader->AddItem(pSpacer);

    m_pHostLabel = AddLabel(m_pHeader, DUI_T(""), DUI_T("system_12"), m_pal.textHint);
    m_pHostLabel->SetAttribute(DUI_T("width"), DUI_T("auto"));
    m_pHostLabel->SetAttribute(DUI_T("text_align"), DUI_T("right,vcenter"));
    DString host = m_hostName.empty() ? DString(DUI_T("本机")) : DString(m_hostName.c_str());
    if (m_cpu.ncpu > 0) {
        host += DUI_T(" · ") + Num(m_cpu.ncpu) + DUI_T(" 核");
    }
    if (m_mem.total > 0) {
        host += DUI_T(" · ") + FormatBytes(m_mem.total) + DUI_T(" 内存");
    }
    m_pHostLabel->SetText(host);
}

void ActivityForm::BuildProcessCard(ui::VBox* pRoot)
{
    ui::VBox* pCard = new ui::VBox(this);
    pCard->SetAttribute(DUI_T("height"), Num(30 + kProcessRows * 22));
    pCard->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    pCard->SetBkColor(m_pal.cardBg);
    pCard->SetBorderColor(m_pal.cardBorder);
    pCard->SetAttribute(DUI_T("border_size"), DUI_T("1"));
    pCard->SetAttribute(DUI_T("padding"), DUI_T("6,0,6,4"));
    pCard->SetAttribute(DUI_T("margin"), DUI_T("0,8,0,0"));
    SetRadius(pCard, 10, false);
    pRoot->AddItem(pCard);

    // Column titles, each one a button that sorts by its column.
    ui::HBox* pHeaderRow = new ui::HBox(this);
    pHeaderRow->SetAttribute(DUI_T("height"), DUI_T("26"));
    pHeaderRow->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    pHeaderRow->SetBkColor(m_pal.headerBg);
    pHeaderRow->SetBorderColor(m_pal.hairline);
    pHeaderRow->SetAttribute(DUI_T("bottom_border_size"), DUI_T("1"));
    pHeaderRow->SetAttribute(DUI_T("padding"), DUI_T("6,0,6,0"));
    SetRadius(pHeaderRow, 6, false);
    pCard->AddItem(pHeaderRow);

    for (int c = 0; c < kColCount; ++c) {
        ui::Button* pColumn = new ui::Button(this);
        pColumn->SetAttribute(DUI_T("font"), DUI_T("system_12"));
        pColumn->SetStateTextColor(ui::kControlStateNormal, m_pal.textHint);
        pColumn->SetAttribute(DUI_T("text_align"), DString(kColumnDefs[c].align));
        pColumn->SetAttribute(DUI_T("width"), DString(kColumnDefs[c].width));
        pColumn->SetAttribute(DUI_T("height"), DUI_T("24"));
        pColumn->SetAttribute(DUI_T("cursor_type"), DUI_T("hand"));
        pColumn->SetStateColor(ui::kControlStateNormal, DUI_T("#00000000"));
        pColumn->SetStateColor(ui::kControlStateHot, m_pal.hairline);
        SetRadius(pColumn, 5, true);
        const int column = c;
        pColumn->AttachClick([this, column](const ui::EventArgs& /*args*/) {
            SetSortColumn(column);
            return true;
        });
        pHeaderRow->AddItem(pColumn);
        m_pColumnButtons[c] = pColumn;
    }
    m_pProcessEmpty = AddLabel(pCard, DUI_T("没有可显示的进程"), DUI_T("system_12"),
                               m_pal.textHint);
    m_pProcessEmpty->SetAttribute(DUI_T("height"), DUI_T("0"));
    m_pProcessEmpty->SetAttribute(DUI_T("width"), DUI_T("stretch"));

    // The rows are built once and only ever have their text rewritten: dui
    // lays a control out at build time, so a row created later would have no
    // size and could not be clicked or read.
    for (int r = 0; r < kProcessRows; ++r) {
        ProcessRow row;
        row.box = new ui::HBox(this);
        row.box->SetAttribute(DUI_T("height"), DUI_T("22"));
        row.box->SetAttribute(DUI_T("width"), DUI_T("stretch"));
        row.box->SetAttribute(DUI_T("padding"), DUI_T("6,0,6,0"));
        row.box->SetBkColor((r % 2) != 0 ? m_pal.rowAlternate : DUI_T("#00000000"));
        row.box->SetMouseEnabled(false);
        pCard->AddItem(row.box);

        for (int c = 0; c < kColCount; ++c) {
            ui::Label* pCell = AddLabel(row.box, DUI_T(""), DUI_T("system_12"),
                                        m_pal.textBody);
            pCell->SetAttribute(DUI_T("width"), DString(kColumnDefs[c].width));
            pCell->SetAttribute(DUI_T("text_align"), DString(kColumnDefs[c].align));
            row.cells[c] = pCell;
        }
        m_processRows.push_back(row);
    }

    UpdateColumnTitles();
}

void ActivityForm::BuildStatusBar(ui::VBox* pRoot)
{
    ui::HBox* pBar = new ui::HBox(this);
    pBar->SetAttribute(DUI_T("height"), DUI_T("28"));
    pBar->SetBkColor(m_pal.windowBg);
    pBar->SetAttribute(DUI_T("padding"), DUI_T("16,0,16,0"));
    pRoot->AddItem(pBar);

    m_pStatusLeft = AddLabel(pBar, DUI_T(""), DUI_T("system_12"), m_pal.textHint);
    m_pStatusLeft->SetAttribute(DUI_T("width"), DUI_T("stretch"));

    m_pStatusRight = AddLabel(pBar, DUI_T("每 1 秒刷新"), DUI_T("system_12"),
                              m_pal.textHint);
    m_pStatusRight->SetAttribute(DUI_T("width"), DUI_T("auto"));
    m_pStatusRight->SetAttribute(DUI_T("text_align"), DUI_T("right,vcenter"));
}

// ---------------------------------------------------------------------------
// Panels
// ---------------------------------------------------------------------------
void ActivityForm::ShowPanel(int index)
{
    if (m_pPanelHost == nullptr) {
        return;
    }
    // Removing the panel's children is safe here: the buttons that call this
    // live in the header, not in the panel being emptied.
    m_pPanelHost->RemoveAllItems();

    m_pCpuGraph = nullptr;
    m_pCpuLoad = nullptr;
    m_pCpuLegend.clear();
    m_pCoreBars.clear();
    m_pCoreReadings.clear();
    m_pMemGraph = nullptr;
    m_pMemHeadline = nullptr;
    m_pMemValues.clear();
    m_diskRows.clear();
    m_pDiskEmpty = nullptr;
    m_pNetDown = nullptr;
    m_pNetUp = nullptr;
    m_pNetHeadline = nullptr;
    m_netRows.clear();
    m_pNetEmpty = nullptr;

    switch (index) {
    case kPanelMemory:  BuildMemoryPanel(m_pPanelHost); break;
    case kPanelDisk:    BuildDiskPanel(m_pPanelHost);   break;
    case kPanelNetwork: BuildNetworkPanel(m_pPanelHost); break;
    case kPanelCpu:
    default:            BuildCpuPanel(m_pPanelHost);    break;
    }

    RefreshPanel();
}

void ActivityForm::UpdateTabStyles()
{
    for (int i = 0; i < kPanelCount; ++i) {
        ui::Button* pTab = m_pTabButtons[i];
        if (pTab == nullptr) {
            continue;
        }
        const bool selected = (i == m_panel);
        pTab->SetStateColor(ui::kControlStateNormal,
                            selected ? m_pal.accent : DUI_T("#00000000"));
        pTab->SetStateColor(ui::kControlStateHot,
                            selected ? m_pal.accent : m_pal.hairline);
        pTab->SetStateTextColor(ui::kControlStateNormal,
                                selected ? m_pal.textOnAccent : m_pal.textBody);
    }
}

void ActivityForm::BuildCpuPanel(ui::VBox* pPanel)
{
    ui::HBox* pTitleRow = new ui::HBox(this);
    pTitleRow->SetAttribute(DUI_T("height"), DUI_T("26"));
    pTitleRow->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    pPanel->AddItem(pTitleRow);

    ui::Label* pTitle = AddLabel(pTitleRow, DUI_T("CPU 负载"), DUI_T("system_bold_16"),
                                 m_pal.textStrong);
    pTitle->SetAttribute(DUI_T("width"), DUI_T("stretch"));

    m_pCpuLoad = AddLabel(pTitleRow, DUI_T(""), DUI_T("system_12"), m_pal.textHint);
    m_pCpuLoad->SetAttribute(DUI_T("width"), DUI_T("360"));
    m_pCpuLoad->SetAttribute(DUI_T("text_align"), DUI_T("right,vcenter"));

    ui::VBox* pGraphCard = AddCard(pPanel, DUI_T("stretch"));
    pGraphCard->SetAttribute(DUI_T("margin"), DUI_T("0,4,0,6"));
    m_pCpuGraph = new activity::HistoryGraph(this);
    m_pCpuGraph->SetAttribute(DUI_T("height"), DUI_T("stretch"));
    m_pCpuGraph->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    m_pCpuGraph->SetBkColor(m_pal.cardBg);
    m_pCpuGraph->SetColours(m_pal.cpuUser, WithAlpha(m_pal.cpuUser, "55"),
                            WithAlpha(m_pal.cpuUser, "00"), m_pal.grid);
    pGraphCard->AddItem(m_pCpuGraph);

    // Legend: the readings the graph is made of.
    ui::HBox* pLegend = new ui::HBox(this);
    pLegend->SetAttribute(DUI_T("height"), DUI_T("24"));
    pLegend->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    pPanel->AddItem(pLegend);
    m_pCpuLegend.push_back(AddLegend(pLegend, m_pal.cpuUser, DUI_T("用户 —"),
                                     DUI_T("120")));
    m_pCpuLegend.push_back(AddLegend(pLegend, m_pal.cpuSystem, DUI_T("系统 —"),
                                     DUI_T("120")));
    m_pCpuLegend.push_back(AddLegend(pLegend, m_pal.textHint, DUI_T("空闲 —"),
                                     DUI_T("120")));
    m_pCpuLegend.push_back(AddLegend(pLegend, m_pal.netUp, DUI_T("温度 —"),
                                     DUI_T("stretch")));

    // One row per core, in as many columns as it takes.  Built at the size it
    // will keep: the bars change fill, never geometry.
    const int ncpu = m_cpu.ncpu > 0 ? m_cpu.ncpu : 1;
    const int columns = ncpu > 1 ? kCoreColumns : 1;
    const int rows = (ncpu + columns - 1) / columns;

    ui::HBox* pCores = new ui::HBox(this);
    pCores->SetAttribute(DUI_T("height"), Num(rows * 20));
    pCores->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    pPanel->AddItem(pCores);

    for (int column = 0; column < columns; ++column) {
        ui::VBox* pColumn = new ui::VBox(this);
        pColumn->SetAttribute(DUI_T("width"), DUI_T("stretch"));
        pColumn->SetAttribute(DUI_T("margin"), DUI_T("0,0,8,0"));
        pCores->AddItem(pColumn);

        for (int row = 0; row < rows; ++row) {
            const int core = column * rows + row;
            if (core >= ncpu) {
                break;
            }
            ui::HBox* pRow = new ui::HBox(this);
            pRow->SetAttribute(DUI_T("height"), DUI_T("20"));
            pRow->SetAttribute(DUI_T("width"), DUI_T("stretch"));
            pRow->SetAttribute(DUI_T("child_align"), DUI_T("vcenter"));
            pColumn->AddItem(pRow);

            ui::Label* pName = AddLabel(pRow, DUI_T("CPU ") + Num(core),
                                        DUI_T("system_12"), m_pal.textHint);
            pName->SetAttribute(DUI_T("width"), DUI_T("52"));

            activity::MeterBar* pBar = new activity::MeterBar(this);
            pBar->SetAttribute(DUI_T("width"), DUI_T("stretch"));
            pBar->SetAttribute(DUI_T("height"), DUI_T("8"));
            pBar->SetColours(m_pal.track, m_pal.cpuUser);
            pRow->AddItem(pBar);
            m_pCoreBars.push_back(pBar);

            ui::Label* pReading = AddLabel(pRow, DUI_T("—"), DUI_T("system_12"),
                                           m_pal.textBody);
            pReading->SetAttribute(DUI_T("width"), DUI_T("46"));
            pReading->SetAttribute(DUI_T("text_align"), DUI_T("right,vcenter"));
            m_pCoreReadings.push_back(pReading);
        }
    }
}

void ActivityForm::BuildMemoryPanel(ui::VBox* pPanel)
{
    ui::HBox* pTitleRow = new ui::HBox(this);
    pTitleRow->SetAttribute(DUI_T("height"), DUI_T("26"));
    pTitleRow->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    pPanel->AddItem(pTitleRow);

    ui::Label* pTitle = AddLabel(pTitleRow, DUI_T("内存压力"), DUI_T("system_bold_16"),
                                 m_pal.textStrong);
    pTitle->SetAttribute(DUI_T("width"), DUI_T("stretch"));

    m_pMemHeadline = AddLabel(pTitleRow, DUI_T(""), DUI_T("system_12"), m_pal.textHint);
    m_pMemHeadline->SetAttribute(DUI_T("width"), DUI_T("360"));
    m_pMemHeadline->SetAttribute(DUI_T("text_align"), DUI_T("right,vcenter"));

    ui::VBox* pGraphCard = AddCard(pPanel, DUI_T("stretch"));
    pGraphCard->SetAttribute(DUI_T("margin"), DUI_T("0,4,0,6"));
    m_pMemGraph = new activity::HistoryGraph(this);
    m_pMemGraph->SetAttribute(DUI_T("height"), DUI_T("stretch"));
    m_pMemGraph->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    m_pMemGraph->SetBkColor(m_pal.cardBg);
    m_pMemGraph->SetColours(m_pal.memUsed, WithAlpha(m_pal.memUsed, "55"),
                            WithAlpha(m_pal.memUsed, "00"), m_pal.grid);
    pGraphCard->AddItem(m_pMemGraph);

    // The breakdown, two columns of two rows plus the swap figure.
    ui::HBox* pBreakdown = new ui::HBox(this);
    pBreakdown->SetAttribute(DUI_T("height"), DUI_T("76"));
    pBreakdown->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    pPanel->AddItem(pBreakdown);

    const char* labels[] = { "活跃", "联动", "非活跃", "缓存", "空闲", "交换" };
    const char* colours[] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
    (void)colours;
    for (int i = 0; i < 6; ++i) {
        if (i == 0 || i == 3) {
            ui::VBox* pColumn = new ui::VBox(this);
            pColumn->SetAttribute(DUI_T("width"), DUI_T("stretch"));
            pColumn->SetAttribute(DUI_T("margin"), DUI_T("0,0,12,0"));
            pBreakdown->AddItem(pColumn);
        }
        ui::VBox* pColumn = static_cast<ui::VBox*>(pBreakdown->GetItemAt(
            static_cast<size_t>(i / 3)));
        ui::HBox* pRow = new ui::HBox(this);
        pRow->SetAttribute(DUI_T("height"), DUI_T("24"));
        pRow->SetAttribute(DUI_T("width"), DUI_T("stretch"));
        pRow->SetAttribute(DUI_T("child_align"), DUI_T("vcenter"));
        pColumn->AddItem(pRow);

        ui::Label* pName = AddLabel(pRow, DString(labels[i]), DUI_T("system_12"),
                                    m_pal.textHint);
        pName->SetAttribute(DUI_T("width"), DUI_T("90"));

        ui::Label* pValue = AddLabel(pRow, DUI_T("—"), DUI_T("system_12"),
                                     m_pal.textBody);
        pValue->SetAttribute(DUI_T("width"), DUI_T("stretch"));
        pValue->SetAttribute(DUI_T("text_align"), DUI_T("right,vcenter"));
        m_pMemValues.push_back(pValue);
    }
}

void ActivityForm::BuildDiskPanel(ui::VBox* pPanel)
{
    ui::HBox* pTitleRow = new ui::HBox(this);
    pTitleRow->SetAttribute(DUI_T("height"), DUI_T("26"));
    pTitleRow->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    pPanel->AddItem(pTitleRow);

    ui::Label* pTitle = AddLabel(pTitleRow, DUI_T("宗卷"), DUI_T("system_bold_16"),
                                 m_pal.textStrong);
    pTitle->SetAttribute(DUI_T("width"), DUI_T("stretch"));

    ui::Label* pHint = AddLabel(pTitleRow, DUI_T("已用 / 总容量"), DUI_T("system_12"),
                                m_pal.textHint);
    pHint->SetAttribute(DUI_T("width"), DUI_T("auto"));
    pHint->SetAttribute(DUI_T("text_align"), DUI_T("right,vcenter"));

    for (int i = 0; i < kMaxDiskRows; ++i) {
        ui::VBox* pBlock = new ui::VBox(this);
        pBlock->SetAttribute(DUI_T("height"), DUI_T("56"));
        pBlock->SetAttribute(DUI_T("width"), DUI_T("stretch"));
        pBlock->SetAttribute(DUI_T("margin"), DUI_T("0,6,0,0"));
        pPanel->AddItem(pBlock);

        ui::HBox* pTop = new ui::HBox(this);
        pTop->SetAttribute(DUI_T("height"), DUI_T("22"));
        pTop->SetAttribute(DUI_T("width"), DUI_T("stretch"));
        pTop->SetAttribute(DUI_T("child_align"), DUI_T("vcenter"));
        pBlock->AddItem(pTop);

        DiskRow row;
        row.name = AddLabel(pTop, DUI_T("—"), DUI_T("system_14"), m_pal.textStrong);
        row.name->SetAttribute(DUI_T("width"), DUI_T("stretch"));

        row.detail = AddLabel(pTop, DUI_T(""), DUI_T("system_12"), m_pal.textHint);
        row.detail->SetAttribute(DUI_T("width"), DUI_T("360"));
        row.detail->SetAttribute(DUI_T("text_align"), DUI_T("right,vcenter"));

        row.bar = new activity::MeterBar(this);
        row.bar->SetAttribute(DUI_T("height"), DUI_T("10"));
        row.bar->SetAttribute(DUI_T("width"), DUI_T("stretch"));
        row.bar->SetAttribute(DUI_T("margin"), DUI_T("0,4,0,0"));
        row.bar->SetColours(m_pal.track, m_pal.diskFill);
        pBlock->AddItem(row.bar);

        m_diskRows.push_back(row);
    }

    m_pDiskEmpty = AddLabel(pPanel, DUI_T("没有可显示的文件系统"), DUI_T("system_12"),
                            m_pal.textHint);
    m_pDiskEmpty->SetAttribute(DUI_T("height"), DUI_T("0"));
    m_pDiskEmpty->SetAttribute(DUI_T("width"), DUI_T("stretch"));
}

void ActivityForm::BuildNetworkPanel(ui::VBox* pPanel)
{
    ui::HBox* pTitleRow = new ui::HBox(this);
    pTitleRow->SetAttribute(DUI_T("height"), DUI_T("26"));
    pTitleRow->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    pPanel->AddItem(pTitleRow);

    ui::Label* pTitle = AddLabel(pTitleRow, DUI_T("网络"), DUI_T("system_bold_16"),
                                 m_pal.textStrong);
    pTitle->SetAttribute(DUI_T("width"), DUI_T("stretch"));

    m_pNetHeadline = AddLabel(pTitleRow, DUI_T(""), DUI_T("system_12"), m_pal.textHint);
    m_pNetHeadline->SetAttribute(DUI_T("width"), DUI_T("auto"));
    m_pNetHeadline->SetAttribute(DUI_T("text_align"), DUI_T("right,vcenter"));

    // Two charts, as the macOS window has them: one for what came in and one
    // for what went out, rather than one chart with two series drawn over each
    // other.
    ui::HBox* pCharts = new ui::HBox(this);
    pCharts->SetAttribute(DUI_T("height"), DUI_T("104"));
    pCharts->SetAttribute(DUI_T("width"), DUI_T("stretch"));
    pPanel->AddItem(pCharts);

    struct ChartDef { const char* title; activity::HistoryGraph** target; DString colour; };
    const ChartDef charts[] = {
        { "下行", &m_pNetDown, m_pal.netDown },
        { "上行", &m_pNetUp,   m_pal.netUp   },
    };
    for (int i = 0; i < 2; ++i) {
        ui::VBox* pColumn = new ui::VBox(this);
        pColumn->SetAttribute(DUI_T("width"), DUI_T("stretch"));
        pColumn->SetAttribute(DUI_T("margin"), DString(i == 0 ? "0,0,8,0" : "8,0,0,0"));
        pCharts->AddItem(pColumn);

        ui::Label* pLabel = AddLabel(pColumn, DString(charts[i].title), DUI_T("system_12"),
                                     m_pal.textHint);
        pLabel->SetAttribute(DUI_T("height"), DUI_T("20"));
        pLabel->SetAttribute(DUI_T("width"), DUI_T("stretch"));

        activity::HistoryGraph* pGraph = new activity::HistoryGraph(this);
        pGraph->SetAttribute(DUI_T("height"), DUI_T("stretch"));
        pGraph->SetAttribute(DUI_T("width"), DUI_T("stretch"));
        pGraph->SetAttribute(DUI_T("margin"), DUI_T("0,4,0,0"));
        pGraph->SetBkColor(m_pal.cardBg);
        pGraph->SetColours(charts[i].colour, WithAlpha(charts[i].colour, "55"),
                           WithAlpha(charts[i].colour, "00"), m_pal.grid);
        pColumn->AddItem(pGraph);
        *charts[i].target = pGraph;
    }

    for (int i = 0; i < kMaxNetRows; ++i) {
        ui::VBox* pBlock = new ui::VBox(this);
        pBlock->SetAttribute(DUI_T("height"), DUI_T("38"));
        pBlock->SetAttribute(DUI_T("width"), DUI_T("stretch"));
        pBlock->SetAttribute(DUI_T("margin"), DUI_T("0,6,0,0"));
        pPanel->AddItem(pBlock);

        ui::HBox* pTop = new ui::HBox(this);
        pTop->SetAttribute(DUI_T("height"), DUI_T("20"));
        pTop->SetAttribute(DUI_T("width"), DUI_T("stretch"));
        pTop->SetAttribute(DUI_T("child_align"), DUI_T("vcenter"));
        pBlock->AddItem(pTop);

        NetRow row;
        row.name = AddLabel(pTop, DUI_T("—"), DUI_T("system_14"), m_pal.textStrong);
        row.name->SetAttribute(DUI_T("width"), DUI_T("stretch"));

        row.rate = AddLabel(pTop, DUI_T(""), DUI_T("system_12"), m_pal.textBody);
        row.rate->SetAttribute(DUI_T("width"), DUI_T("420"));
        row.rate->SetAttribute(DUI_T("text_align"), DUI_T("right,vcenter"));

        row.detail = AddLabel(pBlock, DUI_T(""), DUI_T("system_12"), m_pal.textHint);
        row.detail->SetAttribute(DUI_T("height"), DUI_T("18"));
        row.detail->SetAttribute(DUI_T("width"), DUI_T("stretch"));

        m_netRows.push_back(row);
    }

    m_pNetEmpty = AddLabel(pPanel, DUI_T("没有活动的网络接口"), DUI_T("system_12"),
                           m_pal.textHint);
    m_pNetEmpty->SetAttribute(DUI_T("height"), DUI_T("0"));
    m_pNetEmpty->SetAttribute(DUI_T("width"), DUI_T("stretch"));
}

// ---------------------------------------------------------------------------
// Sampling and refresh
// ---------------------------------------------------------------------------
void ActivityForm::SampleAll()
{
    m_sampler.SampleCpu(m_cpu);
    m_sampler.SampleMemory(m_mem);
    m_sampler.SampleProcesses(m_processes);
    m_sampler.SampleDisks(m_disks);
    m_sampler.SampleNetwork(m_network);

    // History keeps the last two minutes of every reading that has a chart.
    const size_t limit = static_cast<size_t>(kHistoryLength);
    const auto push = [limit](std::vector<float>& history, float value) {
        history.push_back(value);
        while (history.size() > limit) {
            history.erase(history.begin());
        }
    };
    float down = 0.0f, up = 0.0f;
    for (const activity::NetInfo& info : m_network) {
        down += static_cast<float>(info.rxRate);
        up += static_cast<float>(info.txRate);
    }
    push(m_cpuHistory, static_cast<float>(m_cpu.busy));
    push(m_memHistory, static_cast<float>(m_mem.used));
    push(m_netDownHistory, down);
    push(m_netUpHistory, up);

    SortProcesses();
}

void ActivityForm::RefreshPanel()
{
    switch (m_panel) {
    case kPanelMemory:  RefreshMemoryPanel();  break;
    case kPanelDisk:    RefreshDiskPanel();    break;
    case kPanelNetwork: RefreshNetworkPanel(); break;
    case kPanelCpu:
    default:            RefreshCpuPanel();     break;
    }
}

void ActivityForm::RefreshCpuPanel()
{
    if (m_pCpuGraph == nullptr) {
        return;
    }
    m_pCpuGraph->SetSamples(m_cpuHistory, 100.0f);

    if (m_pCpuLoad != nullptr) {
        m_pCpuLoad->SetText(
            DUI_T("负载 ") + FormatDouble(m_cpu.load[0], "%.2f") + DUI_T(" ") +
            FormatDouble(m_cpu.load[1], "%.2f") + DUI_T(" ") +
            FormatDouble(m_cpu.load[2], "%.2f") + DUI_T("   ·   运行 ") +
            FormatUptime(m_cpu.uptime));
    }

    if (m_pCpuLegend.size() >= 4) {
        m_pCpuLegend[0]->SetText(DUI_T("用户 ") + FormatDouble(m_cpu.user, "%.1f") + DUI_T("%"));
        m_pCpuLegend[1]->SetText(DUI_T("系统 ") + FormatDouble(m_cpu.sys, "%.1f") + DUI_T("%"));
        m_pCpuLegend[2]->SetText(DUI_T("空闲 ") + FormatDouble(m_cpu.idle, "%.1f") + DUI_T("%"));
        m_pCpuLegend[3]->SetText(
            m_cpu.temperature > 0.0
                ? DUI_T("温度 ") + FormatDouble(m_cpu.temperature, "%.1f") + DUI_T(" °C")
                : DUI_T("温度 不可读"));
    }

    for (size_t i = 0; i < m_pCoreBars.size(); ++i) {
        const double busy = i < m_cpu.coreBusy.size() ? m_cpu.coreBusy[i] : 0.0;
        m_pCoreBars[i]->SetFraction(static_cast<float>(busy / 100.0));
        if (i < m_pCoreReadings.size()) {
            m_pCoreReadings[i]->SetText(FormatDouble(busy, "%.0f") + DUI_T("%"));
        }
    }
}

void ActivityForm::RefreshMemoryPanel()
{
    if (m_pMemGraph == nullptr) {
        return;
    }
    m_pMemGraph->SetSamples(m_memHistory, static_cast<float>(m_mem.total));

    if (m_pMemHeadline != nullptr) {
        m_pMemHeadline->SetText(
            DUI_T("已用 ") + FormatBytes(m_mem.used) + DUI_T(" / ") +
            FormatBytes(m_mem.total) + DUI_T("  (") +
            FormatDouble(m_mem.usedPercent, "%.0f") + DUI_T("%)"));
    }

    const unsigned long long values[] = { m_mem.active, m_mem.wired, m_mem.inactive,
                                          m_mem.cached, m_mem.freeB, m_mem.swapTotal };
    for (size_t i = 0; i < m_pMemValues.size() && i < 6; ++i) {
        m_pMemValues[i]->SetText(FormatBytes(values[i]));
    }
}

void ActivityForm::RefreshDiskPanel()
{
    const size_t shown = (std::min)(m_diskRows.size(), m_disks.size());
    for (size_t i = 0; i < m_diskRows.size(); ++i) {
        DiskRow& row = m_diskRows[i];
        if (i >= shown) {
            row.name->SetText(DUI_T(""));
            row.detail->SetText(DUI_T(""));
            row.bar->SetFraction(0.0f);
            continue;
        }
        const activity::DiskInfo& disk = m_disks[i];
        row.name->SetText(DString(disk.mount.c_str()));
        row.detail->SetText(
            FormatBytes(disk.used) + DUI_T(" / ") + FormatBytes(disk.total) +
            DUI_T("   ·   可用 ") + FormatBytes(disk.avail));
        row.bar->SetColours(m_pal.track,
                            disk.usedPercent >= 90.0 ? m_pal.diskFull : m_pal.diskFill);
        row.bar->SetFraction(static_cast<float>(disk.usedPercent / 100.0));
    }
    if (m_pDiskEmpty != nullptr) {
        m_pDiskEmpty->SetAttribute(DUI_T("height"),
                                   m_disks.empty() ? DUI_T("22") : DUI_T("0"));
    }
}

void ActivityForm::RefreshNetworkPanel()
{
    if (m_pNetDown == nullptr || m_pNetUp == nullptr) {
        return;
    }
    float downPeak = 0.0f, upPeak = 0.0f;
    for (float value : m_netDownHistory) downPeak = (std::max)(downPeak, value);
    for (float value : m_netUpHistory)   upPeak   = (std::max)(upPeak, value);
    // A chart that always fills its height is unreadable, so the scale is the
    // busiest moment in the window with a little headroom -- and never zero,
    // which would divide the whole chart away.
    const float floorScale = 64.0f * 1024.0f;
    m_pNetDown->SetSamples(m_netDownHistory, (std::max)(downPeak * 1.15f, floorScale));
    m_pNetUp->SetSamples(m_netUpHistory, (std::max)(upPeak * 1.15f, floorScale));

    double down = 0.0, up = 0.0;
    for (const activity::NetInfo& info : m_network) {
        down += info.rxRate;
        up += info.txRate;
    }
    if (m_pNetHeadline != nullptr) {
        m_pNetHeadline->SetText(DUI_T("↓ ") + FormatRate(down) + DUI_T("   ↑ ") +
                                FormatRate(up));
    }

    const size_t shown = (std::min)(m_netRows.size(), m_network.size());
    for (size_t i = 0; i < m_netRows.size(); ++i) {
        NetRow& row = m_netRows[i];
        if (i >= shown) {
            row.name->SetText(DUI_T(""));
            row.rate->SetText(DUI_T(""));
            row.detail->SetText(DUI_T(""));
            continue;
        }
        const activity::NetInfo& info = m_network[i];
        row.name->SetText(DString(info.name.c_str()) +
                          (info.up ? DUI_T("  已连接") : DUI_T("  未连接")));
        row.rate->SetText(DUI_T("↓ ") + FormatRate(info.rxRate) + DUI_T("   ↑ ") +
                          FormatRate(info.txRate));
        DString detail = DUI_T("累计 收 ") + FormatBytes(info.rxBytes) + DUI_T(" · 发 ") +
                         FormatBytes(info.txBytes);
        for (const std::string& address : info.addresses) {
            detail += DUI_T("   ·   ") + DString(address.c_str());
        }
        row.detail->SetText(detail);
    }
    if (m_pNetEmpty != nullptr) {
        m_pNetEmpty->SetAttribute(DUI_T("height"),
                                  m_network.empty() ? DUI_T("22") : DUI_T("0"));
    }
}

void ActivityForm::RefreshProcessTable()
{
    const size_t shown = (std::min)(m_processRows.size(), m_processes.size());
    for (size_t i = 0; i < m_processRows.size(); ++i) {
        if (i < shown) {
            FillProcessRow(i, m_processes[i]);
            continue;
        }
        for (int c = 0; c < kColCount; ++c) {
            m_processRows[i].cells[c]->SetText(DUI_T(""));
        }
    }
    if (m_pProcessEmpty != nullptr) {
        m_pProcessEmpty->SetAttribute(DUI_T("height"),
                                      m_processes.empty() ? DUI_T("22") : DUI_T("0"));
    }
}

void ActivityForm::FillProcessRow(size_t row, const activity::ProcessInfo& info)
{
    if (row >= m_processRows.size()) {
        return;
    }
    ProcessRow& cells = m_processRows[row];
    cells.cells[kColName]->SetText(DString(info.name.c_str()));
    cells.cells[kColPid]->SetText(Num(info.pid));
    cells.cells[kColCpu]->SetText(FormatDouble(info.cpu, "%.1f"));
    cells.cells[kColTime]->SetText(FormatDuration(info.cpuSeconds));
    cells.cells[kColMemory]->SetText(FormatBytes(info.rss));
    cells.cells[kColThreads]->SetText(Num(info.threads));
    cells.cells[kColState]->SetText(DString(activity::Sampler::StateText(info.state).c_str()));

    // A process eating a core or more is worth noticing without reading the
    // number.
    cells.cells[kColCpu]->SetStateTextColor(
        ui::kControlStateNormal, info.cpu >= 50.0 ? m_pal.cpuSystem : m_pal.textBody);
}

void ActivityForm::RefreshStatusBar()
{
    long long threads = 0;
    for (const activity::ProcessInfo& info : m_processes) {
        threads += info.threads;
    }
    if (m_pStatusLeft != nullptr) {
        m_pStatusLeft->SetText(Num(static_cast<long long>(m_processes.size())) +
                               DUI_T(" 个进程   ·   ") + Num(threads) + DUI_T(" 个线程"));
    }
}

// ---------------------------------------------------------------------------
// Process table sorting
// ---------------------------------------------------------------------------
void ActivityForm::SortProcesses()
{
    const int column = m_sortColumn;
    const bool ascending = m_sortAscending;
    std::stable_sort(m_processes.begin(), m_processes.end(),
                     [column, ascending](const activity::ProcessInfo& a,
                                         const activity::ProcessInfo& b) {
        bool less;
        switch (column) {
        case kColName:    less = a.name < b.name; break;
        case kColPid:     less = a.pid < b.pid; break;
        case kColTime:    less = a.cpuSeconds < b.cpuSeconds; break;
        case kColMemory:  less = a.rss < b.rss; break;
        case kColThreads: less = a.threads < b.threads; break;
        case kColState:   less = a.state < b.state; break;
        case kColCpu:
        default:          less = a.cpu < b.cpu; break;
        }
        if (less == ascending) {
            return true;
        }
        // Ties keep the busiest first whichever way the column is sorted: a
        // table sorted by name should still lead with what is running.
        if (column != kColCpu && a.cpu != b.cpu) {
            return a.cpu > b.cpu;
        }
        return false;
    });
}

void ActivityForm::SetSortColumn(int column)
{
    if (column == m_sortColumn) {
        m_sortAscending = !m_sortAscending;
    } else {
        m_sortColumn = column;
        // Numbers are most useful biggest-first; names read best A-Z.
        m_sortAscending = (column == kColName || column == kColState);
    }
    SortProcesses();
    RefreshProcessTable();
    UpdateColumnTitles();
}

void ActivityForm::UpdateColumnTitles()
{
    for (int c = 0; c < kColCount; ++c) {
        if (m_pColumnButtons[c] == nullptr) {
            continue;
        }
        DString title = DString(kColumnDefs[c].title);
        if (c == m_sortColumn) {
            title += m_sortAscending ? DUI_T(" ▲") : DUI_T(" ▼");
        }
        m_pColumnButtons[c]->SetText(title);
        m_pColumnButtons[c]->SetStateTextColor(
            ui::kControlStateNormal, c == m_sortColumn ? m_pal.textBody : m_pal.textHint);
    }
}

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------
DString ActivityForm::FormatBytes(unsigned long long bytes)
{
    const double value = static_cast<double>(bytes);
    if (bytes >= 1024ULL * 1024 * 1024) {
        return FormatDouble(value / (1024.0 * 1024.0 * 1024.0), "%.1f GB");
    }
    if (bytes >= 1024ULL * 1024) {
        return FormatDouble(value / (1024.0 * 1024.0), "%.0f MB");
    }
    if (bytes >= 1024) {
        return FormatDouble(value / 1024.0, "%.0f KB");
    }
    return Num(static_cast<long long>(bytes)) + DUI_T(" B");
}

DString ActivityForm::FormatRate(double bytesPerSecond)
{
    if (bytesPerSecond >= 1024.0 * 1024.0) {
        return FormatDouble(bytesPerSecond / (1024.0 * 1024.0), "%.1f MB/s");
    }
    if (bytesPerSecond >= 1024.0) {
        return FormatDouble(bytesPerSecond / 1024.0, "%.1f KB/s");
    }
    return FormatDouble(bytesPerSecond, "%.0f B/s");
}

DString ActivityForm::FormatDuration(double seconds)
{
    const long long total = static_cast<long long>(seconds);
    const long long hours = total / 3600;
    const long long minutes = (total % 3600) / 60;
    const long long secs = total % 60;
    char buffer[32];
    if (hours > 0) {
        std::snprintf(buffer, sizeof(buffer), "%lld:%02lld:%02lld", hours, minutes, secs);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%lld:%02lld", minutes, secs);
    }
    return DString(buffer);
}

DString ActivityForm::FormatUptime(long seconds)
{
    const long days = seconds / 86400;
    const long hours = (seconds % 86400) / 3600;
    const long minutes = (seconds % 3600) / 60;
    char buffer[64];
    if (days > 0) {
        std::snprintf(buffer, sizeof(buffer), "%ld 天 %ld:%02ld", days, hours, minutes);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%ld:%02ld", hours, minutes);
    }
    return DString(buffer);
}
