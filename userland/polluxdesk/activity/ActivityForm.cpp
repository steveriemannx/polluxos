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

U8String Num(long long value)
{
    return ui::StringUtil::Printf("%lld", value);
}

U8String FormatDouble(double value, const char* format)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), format, value);
    return U8String(buffer);
}

// "#AARRGGBB" with a different alpha: the fill under a graph line is the line's
// own colour, faded.
U8String WithAlpha(const U8String& colour, const char* alpha)
{
    if (colour.size() != 9 || colour[0] != '#') {
        return colour;
    }
    return U8String("#") + U8String(alpha) + colour.substr(3);
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

U8String ActivityForm::GetSkinFolder() { return ""; }
U8String ActivityForm::GetSkinFile()   { return ""; }

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
    m_pal.accent = U8String(pollux::AccentHex(m_settings));
    m_pal.textOnAccent = "#FFFFFFFF";

    if (dark) {
        m_pal.windowBg      = "#FF1C1C1E";
        m_pal.cardBg        = "#FF2C2C2E";
        m_pal.cardBorder    = "#26FFFFFF";
        m_pal.headerBg      = "#FF242426";
        m_pal.rowAlternate  = "#0DFFFFFF";
        m_pal.hairline      = "#26FFFFFF";
        m_pal.textStrong    = "#FFF5F5F7";
        m_pal.textBody      = "#FFD8D8DC";
        m_pal.textHint      = "#FF98989D";
        m_pal.track         = "#33FFFFFF";
        m_pal.grid          = "#1FFFFFFF";
        m_pal.tabTrack      = "#1FFFFFFF";
        m_pal.cpuUser       = "#FF0A84FF";
        m_pal.cpuSystem     = "#FFFF453A";
        m_pal.memUsed       = "#FF32D74B";
        m_pal.diskFill      = "#FF0A84FF";
        m_pal.diskFull      = "#FFFF453A";
        m_pal.netDown       = "#FF32D74B";
        m_pal.netUp         = "#FFFF9F0A";
    } else {
        m_pal.windowBg      = "#FFF2F2F7";
        m_pal.cardBg        = "#FFFFFFFF";
        m_pal.cardBorder    = "#22000000";
        m_pal.headerBg      = "#FFF7F7F9";
        m_pal.rowAlternate  = "#0A000000";
        m_pal.hairline      = "#22000000";
        m_pal.textStrong    = "#FF1D1D1F";
        m_pal.textBody      = "#FF3A3A3C";
        m_pal.textHint      = "#FF8E8E93";
        m_pal.track         = "#1F000000";
        m_pal.grid          = "#14000000";
        m_pal.tabTrack      = "#14000000";
        m_pal.cpuUser       = "#FF007AFF";
        m_pal.cpuSystem     = "#FFFF3B30";
        m_pal.memUsed       = "#FF34C759";
        m_pal.diskFill      = "#FF007AFF";
        m_pal.diskFull      = "#FFFF3B30";
        m_pal.netDown       = "#FF34C759";
        m_pal.netUp         = "#FFFF9500";
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
    pControl->SetAttribute("border_round",
                           ui::StringUtil::Printf("%d,%d", radius, radius));
}

ui::Label* ActivityForm::AddLabel(ui::Box* pParent, const U8String& text,
                                  const U8String& font, const U8String& colour)
{
    ui::Label* pLabel = new ui::Label(this);
    pLabel->SetText(text);
    pLabel->SetAttribute("font", font);
    // SetStateTextColor rather than the "text_color" attribute: that attribute
    // does not exist for a label, and dui falls back to a default colour, which
    // is how a window ends up unreadable the moment the desktop goes dark.
    pLabel->SetStateTextColor(ui::kControlStateNormal, colour);
    pLabel->SetAttribute("text_align", "left,vcenter");
    pLabel->SetMouseEnabled(false);
    pParent->AddItem(pLabel);
    return pLabel;
}

ui::VBox* ActivityForm::AddCard(ui::Box* pParent, const U8String& height)
{
    ui::VBox* pCard = new ui::VBox(this);
    pCard->SetAttribute("height", height);
    pCard->SetAttribute("width", "stretch");
    pCard->SetBkColor(m_pal.cardBg);
    pCard->SetBorderColor(m_pal.cardBorder);
    pCard->SetAttribute("border_size", "1");
    pCard->SetAttribute("padding", "8,6,8,6");
    SetRadius(pCard, 10, false);
    pParent->AddItem(pCard);
    return pCard;
}

ui::Label* ActivityForm::AddLegend(ui::Box* pParent, const U8String& colour,
                                   const U8String& text, const U8String& width)
{
    ui::HBox* pItem = new ui::HBox(this);
    pItem->SetAttribute("width", width);
    pItem->SetAttribute("height", "22");
    pItem->SetAttribute("child_align", "vcenter");

    ui::Control* pDot = new ui::Control(this);
    pDot->SetAttribute("width", "8");
    pDot->SetAttribute("height", "8");
    pDot->SetAttribute("margin", "0,0,6,0");
    pDot->SetBkColor(colour);
    SetRadius(pDot, 4, false);
    pDot->SetMouseEnabled(false);
    pItem->AddItem(pDot);

    ui::Label* pLabel = AddLabel(pItem, text, "system_12", m_pal.textBody);
    pLabel->SetAttribute("width", "stretch");
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
    pRoot->SetBorderColor("#00000000");
    pRoot->SetAttribute("border_size", "0");
    pRoot->SetAttribute("padding", "0,0,0,0");

    BuildHeader(pRoot);

    ui::Control* pHairline = new ui::Control(this);
    pHairline->SetAttribute("height", "1");
    pHairline->SetAttribute("width", "stretch");
    pHairline->SetBkColor(m_pal.hairline);
    pHairline->SetMouseEnabled(false);
    pRoot->AddItem(pHairline);

    ui::VBox* pBody = new ui::VBox(this);
    pBody->SetAttribute("height", "stretch");
    pBody->SetAttribute("padding", "16,10,16,6");
    pRoot->AddItem(pBody);

    m_pPanelHost = new ui::VBox(this);
    m_pPanelHost->SetAttribute("height", "stretch");
    m_pPanelHost->SetAttribute("width", "stretch");
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
    m_pHeader->SetAttribute("height", "56");
    m_pHeader->SetBkColor(m_pal.windowBg);
    m_pHeader->SetAttribute("padding", "16,0,16,0");
    m_pHeader->SetAttribute("child_align", "vcenter");
    pRoot->AddItem(m_pHeader);

    // A segmented control, built out of buttons: dui's own Combo opens a second
    // window, and this backend routes input to one window per process.
    m_pTabs = new ui::HBox(this);
    m_pTabs->SetAttribute("width", "auto");
    m_pTabs->SetAttribute("height", "32");
    m_pTabs->SetBkColor(m_pal.tabTrack);
    m_pTabs->SetAttribute("padding", "2,2,2,2");
    m_pTabs->SetAttribute("child_align", "vcenter");
    SetRadius(m_pTabs, 8, false);
    m_pHeader->AddItem(m_pTabs);

    for (int i = 0; i < kPanelCount; ++i) {
        ui::Button* pTab = new ui::Button(this);
        pTab->SetText(U8String(kPanelTitles[i]));
        pTab->SetAttribute("font", "system_14");
        pTab->SetAttribute("width", "78");
        pTab->SetAttribute("height", "26");
        pTab->SetAttribute("margin", "2,0,2,0");
        pTab->SetAttribute("text_align", "hcenter,vcenter");
        pTab->SetAttribute("cursor_type", "hand");
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
    pSpacer->SetAttribute("width", "stretch");
    pSpacer->SetMouseEnabled(false);
    m_pHeader->AddItem(pSpacer);

    m_pHostLabel = AddLabel(m_pHeader, "", "system_12", m_pal.textHint);
    m_pHostLabel->SetAttribute("width", "auto");
    m_pHostLabel->SetAttribute("text_align", "right,vcenter");
    U8String host = m_hostName.empty() ? U8String("本机") : U8String(m_hostName.c_str());
    if (m_cpu.ncpu > 0) {
        host += " · " + Num(m_cpu.ncpu) + " 核";
    }
    if (m_mem.total > 0) {
        host += " · " + FormatBytes(m_mem.total) + " 内存";
    }
    m_pHostLabel->SetText(host);
}

void ActivityForm::BuildProcessCard(ui::VBox* pRoot)
{
    ui::VBox* pCard = new ui::VBox(this);
    pCard->SetAttribute("height", Num(30 + kProcessRows * 22));
    pCard->SetAttribute("width", "stretch");
    pCard->SetBkColor(m_pal.cardBg);
    pCard->SetBorderColor(m_pal.cardBorder);
    pCard->SetAttribute("border_size", "1");
    pCard->SetAttribute("padding", "6,0,6,4");
    pCard->SetAttribute("margin", "0,8,0,0");
    SetRadius(pCard, 10, false);
    pRoot->AddItem(pCard);

    // Column titles, each one a button that sorts by its column.
    ui::HBox* pHeaderRow = new ui::HBox(this);
    pHeaderRow->SetAttribute("height", "26");
    pHeaderRow->SetAttribute("width", "stretch");
    pHeaderRow->SetBkColor(m_pal.headerBg);
    pHeaderRow->SetBorderColor(m_pal.hairline);
    pHeaderRow->SetAttribute("bottom_border_size", "1");
    pHeaderRow->SetAttribute("padding", "6,0,6,0");
    SetRadius(pHeaderRow, 6, false);
    pCard->AddItem(pHeaderRow);

    for (int c = 0; c < kColCount; ++c) {
        ui::Button* pColumn = new ui::Button(this);
        pColumn->SetAttribute("font", "system_12");
        pColumn->SetStateTextColor(ui::kControlStateNormal, m_pal.textHint);
        pColumn->SetAttribute("text_align", U8String(kColumnDefs[c].align));
        pColumn->SetAttribute("width", U8String(kColumnDefs[c].width));
        pColumn->SetAttribute("height", "24");
        pColumn->SetAttribute("cursor_type", "hand");
        pColumn->SetStateColor(ui::kControlStateNormal, "#00000000");
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
    m_pProcessEmpty = AddLabel(pCard, "没有可显示的进程", "system_12",
                               m_pal.textHint);
    m_pProcessEmpty->SetAttribute("height", "0");
    m_pProcessEmpty->SetAttribute("width", "stretch");

    // The rows are built once and only ever have their text rewritten: dui
    // lays a control out at build time, so a row created later would have no
    // size and could not be clicked or read.
    for (int r = 0; r < kProcessRows; ++r) {
        ProcessRow row;
        row.box = new ui::HBox(this);
        row.box->SetAttribute("height", "22");
        row.box->SetAttribute("width", "stretch");
        row.box->SetAttribute("padding", "6,0,6,0");
        row.box->SetBkColor((r % 2) != 0 ? m_pal.rowAlternate : "#00000000");
        row.box->SetMouseEnabled(false);
        pCard->AddItem(row.box);

        for (int c = 0; c < kColCount; ++c) {
            ui::Label* pCell = AddLabel(row.box, "", "system_12",
                                        m_pal.textBody);
            pCell->SetAttribute("width", U8String(kColumnDefs[c].width));
            pCell->SetAttribute("text_align", U8String(kColumnDefs[c].align));
            row.cells[c] = pCell;
        }
        m_processRows.push_back(row);
    }

    UpdateColumnTitles();
}

void ActivityForm::BuildStatusBar(ui::VBox* pRoot)
{
    ui::HBox* pBar = new ui::HBox(this);
    pBar->SetAttribute("height", "28");
    pBar->SetBkColor(m_pal.windowBg);
    pBar->SetAttribute("padding", "16,0,16,0");
    pRoot->AddItem(pBar);

    m_pStatusLeft = AddLabel(pBar, "", "system_12", m_pal.textHint);
    m_pStatusLeft->SetAttribute("width", "stretch");

    m_pStatusRight = AddLabel(pBar, "每 1 秒刷新", "system_12",
                              m_pal.textHint);
    m_pStatusRight->SetAttribute("width", "auto");
    m_pStatusRight->SetAttribute("text_align", "right,vcenter");
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
                            selected ? m_pal.accent : "#00000000");
        pTab->SetStateColor(ui::kControlStateHot,
                            selected ? m_pal.accent : m_pal.hairline);
        pTab->SetStateTextColor(ui::kControlStateNormal,
                                selected ? m_pal.textOnAccent : m_pal.textBody);
    }
}

void ActivityForm::BuildCpuPanel(ui::VBox* pPanel)
{
    ui::HBox* pTitleRow = new ui::HBox(this);
    pTitleRow->SetAttribute("height", "26");
    pTitleRow->SetAttribute("width", "stretch");
    pPanel->AddItem(pTitleRow);

    ui::Label* pTitle = AddLabel(pTitleRow, "CPU 负载", "system_bold_16",
                                 m_pal.textStrong);
    pTitle->SetAttribute("width", "stretch");

    m_pCpuLoad = AddLabel(pTitleRow, "", "system_12", m_pal.textHint);
    m_pCpuLoad->SetAttribute("width", "360");
    m_pCpuLoad->SetAttribute("text_align", "right,vcenter");

    ui::VBox* pGraphCard = AddCard(pPanel, "stretch");
    pGraphCard->SetAttribute("margin", "0,4,0,6");
    m_pCpuGraph = new activity::HistoryGraph(this);
    m_pCpuGraph->SetAttribute("height", "stretch");
    m_pCpuGraph->SetAttribute("width", "stretch");
    m_pCpuGraph->SetBkColor(m_pal.cardBg);
    m_pCpuGraph->SetColours(m_pal.cpuUser, WithAlpha(m_pal.cpuUser, "55"),
                            WithAlpha(m_pal.cpuUser, "00"), m_pal.grid);
    pGraphCard->AddItem(m_pCpuGraph);

    // Legend: the readings the graph is made of.
    ui::HBox* pLegend = new ui::HBox(this);
    pLegend->SetAttribute("height", "24");
    pLegend->SetAttribute("width", "stretch");
    pPanel->AddItem(pLegend);
    m_pCpuLegend.push_back(AddLegend(pLegend, m_pal.cpuUser, "用户 —",
                                     "120"));
    m_pCpuLegend.push_back(AddLegend(pLegend, m_pal.cpuSystem, "系统 —",
                                     "120"));
    m_pCpuLegend.push_back(AddLegend(pLegend, m_pal.textHint, "空闲 —",
                                     "120"));
    m_pCpuLegend.push_back(AddLegend(pLegend, m_pal.netUp, "温度 —",
                                     "stretch"));

    // One row per core, in as many columns as it takes.  Built at the size it
    // will keep: the bars change fill, never geometry.
    const int ncpu = m_cpu.ncpu > 0 ? m_cpu.ncpu : 1;
    const int columns = ncpu > 1 ? kCoreColumns : 1;
    const int rows = (ncpu + columns - 1) / columns;

    ui::HBox* pCores = new ui::HBox(this);
    pCores->SetAttribute("height", Num(rows * 20));
    pCores->SetAttribute("width", "stretch");
    pPanel->AddItem(pCores);

    for (int column = 0; column < columns; ++column) {
        ui::VBox* pColumn = new ui::VBox(this);
        pColumn->SetAttribute("width", "stretch");
        pColumn->SetAttribute("margin", "0,0,8,0");
        pCores->AddItem(pColumn);

        for (int row = 0; row < rows; ++row) {
            const int core = column * rows + row;
            if (core >= ncpu) {
                break;
            }
            ui::HBox* pRow = new ui::HBox(this);
            pRow->SetAttribute("height", "20");
            pRow->SetAttribute("width", "stretch");
            pRow->SetAttribute("child_align", "vcenter");
            pColumn->AddItem(pRow);

            ui::Label* pName = AddLabel(pRow, "CPU " + Num(core),
                                        "system_12", m_pal.textHint);
            pName->SetAttribute("width", "52");

            activity::MeterBar* pBar = new activity::MeterBar(this);
            pBar->SetAttribute("width", "stretch");
            pBar->SetAttribute("height", "8");
            pBar->SetColours(m_pal.track, m_pal.cpuUser);
            pRow->AddItem(pBar);
            m_pCoreBars.push_back(pBar);

            ui::Label* pReading = AddLabel(pRow, "—", "system_12",
                                           m_pal.textBody);
            pReading->SetAttribute("width", "46");
            pReading->SetAttribute("text_align", "right,vcenter");
            m_pCoreReadings.push_back(pReading);
        }
    }
}

void ActivityForm::BuildMemoryPanel(ui::VBox* pPanel)
{
    ui::HBox* pTitleRow = new ui::HBox(this);
    pTitleRow->SetAttribute("height", "26");
    pTitleRow->SetAttribute("width", "stretch");
    pPanel->AddItem(pTitleRow);

    ui::Label* pTitle = AddLabel(pTitleRow, "内存压力", "system_bold_16",
                                 m_pal.textStrong);
    pTitle->SetAttribute("width", "stretch");

    m_pMemHeadline = AddLabel(pTitleRow, "", "system_12", m_pal.textHint);
    m_pMemHeadline->SetAttribute("width", "360");
    m_pMemHeadline->SetAttribute("text_align", "right,vcenter");

    ui::VBox* pGraphCard = AddCard(pPanel, "stretch");
    pGraphCard->SetAttribute("margin", "0,4,0,6");
    m_pMemGraph = new activity::HistoryGraph(this);
    m_pMemGraph->SetAttribute("height", "stretch");
    m_pMemGraph->SetAttribute("width", "stretch");
    m_pMemGraph->SetBkColor(m_pal.cardBg);
    m_pMemGraph->SetColours(m_pal.memUsed, WithAlpha(m_pal.memUsed, "55"),
                            WithAlpha(m_pal.memUsed, "00"), m_pal.grid);
    pGraphCard->AddItem(m_pMemGraph);

    // The breakdown, two columns of two rows plus the swap figure.
    ui::HBox* pBreakdown = new ui::HBox(this);
    pBreakdown->SetAttribute("height", "76");
    pBreakdown->SetAttribute("width", "stretch");
    pPanel->AddItem(pBreakdown);

    const char* labels[] = { "活跃", "联动", "非活跃", "缓存", "空闲", "交换" };
    const char* colours[] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
    (void)colours;
    for (int i = 0; i < 6; ++i) {
        if (i == 0 || i == 3) {
            ui::VBox* pColumn = new ui::VBox(this);
            pColumn->SetAttribute("width", "stretch");
            pColumn->SetAttribute("margin", "0,0,12,0");
            pBreakdown->AddItem(pColumn);
        }
        ui::VBox* pColumn = static_cast<ui::VBox*>(pBreakdown->GetItemAt(
            static_cast<size_t>(i / 3)));
        ui::HBox* pRow = new ui::HBox(this);
        pRow->SetAttribute("height", "24");
        pRow->SetAttribute("width", "stretch");
        pRow->SetAttribute("child_align", "vcenter");
        pColumn->AddItem(pRow);

        ui::Label* pName = AddLabel(pRow, U8String(labels[i]), "system_12",
                                    m_pal.textHint);
        pName->SetAttribute("width", "90");

        ui::Label* pValue = AddLabel(pRow, "—", "system_12",
                                     m_pal.textBody);
        pValue->SetAttribute("width", "stretch");
        pValue->SetAttribute("text_align", "right,vcenter");
        m_pMemValues.push_back(pValue);
    }
}

void ActivityForm::BuildDiskPanel(ui::VBox* pPanel)
{
    ui::HBox* pTitleRow = new ui::HBox(this);
    pTitleRow->SetAttribute("height", "26");
    pTitleRow->SetAttribute("width", "stretch");
    pPanel->AddItem(pTitleRow);

    ui::Label* pTitle = AddLabel(pTitleRow, "宗卷", "system_bold_16",
                                 m_pal.textStrong);
    pTitle->SetAttribute("width", "stretch");

    ui::Label* pHint = AddLabel(pTitleRow, "已用 / 总容量", "system_12",
                                m_pal.textHint);
    pHint->SetAttribute("width", "auto");
    pHint->SetAttribute("text_align", "right,vcenter");

    for (int i = 0; i < kMaxDiskRows; ++i) {
        ui::VBox* pBlock = new ui::VBox(this);
        pBlock->SetAttribute("height", "56");
        pBlock->SetAttribute("width", "stretch");
        pBlock->SetAttribute("margin", "0,6,0,0");
        pPanel->AddItem(pBlock);

        ui::HBox* pTop = new ui::HBox(this);
        pTop->SetAttribute("height", "22");
        pTop->SetAttribute("width", "stretch");
        pTop->SetAttribute("child_align", "vcenter");
        pBlock->AddItem(pTop);

        DiskRow row;
        row.name = AddLabel(pTop, "—", "system_14", m_pal.textStrong);
        row.name->SetAttribute("width", "stretch");

        row.detail = AddLabel(pTop, "", "system_12", m_pal.textHint);
        row.detail->SetAttribute("width", "360");
        row.detail->SetAttribute("text_align", "right,vcenter");

        row.bar = new activity::MeterBar(this);
        row.bar->SetAttribute("height", "10");
        row.bar->SetAttribute("width", "stretch");
        row.bar->SetAttribute("margin", "0,4,0,0");
        row.bar->SetColours(m_pal.track, m_pal.diskFill);
        pBlock->AddItem(row.bar);

        m_diskRows.push_back(row);
    }

    m_pDiskEmpty = AddLabel(pPanel, "没有可显示的文件系统", "system_12",
                            m_pal.textHint);
    m_pDiskEmpty->SetAttribute("height", "0");
    m_pDiskEmpty->SetAttribute("width", "stretch");
}

void ActivityForm::BuildNetworkPanel(ui::VBox* pPanel)
{
    ui::HBox* pTitleRow = new ui::HBox(this);
    pTitleRow->SetAttribute("height", "26");
    pTitleRow->SetAttribute("width", "stretch");
    pPanel->AddItem(pTitleRow);

    ui::Label* pTitle = AddLabel(pTitleRow, "网络", "system_bold_16",
                                 m_pal.textStrong);
    pTitle->SetAttribute("width", "stretch");

    m_pNetHeadline = AddLabel(pTitleRow, "", "system_12", m_pal.textHint);
    m_pNetHeadline->SetAttribute("width", "auto");
    m_pNetHeadline->SetAttribute("text_align", "right,vcenter");

    // Two charts, as the macOS window has them: one for what came in and one
    // for what went out, rather than one chart with two series drawn over each
    // other.
    ui::HBox* pCharts = new ui::HBox(this);
    pCharts->SetAttribute("height", "104");
    pCharts->SetAttribute("width", "stretch");
    pPanel->AddItem(pCharts);

    struct ChartDef { const char* title; activity::HistoryGraph** target; U8String colour; };
    const ChartDef charts[] = {
        { "下行", &m_pNetDown, m_pal.netDown },
        { "上行", &m_pNetUp,   m_pal.netUp   },
    };
    for (int i = 0; i < 2; ++i) {
        ui::VBox* pColumn = new ui::VBox(this);
        pColumn->SetAttribute("width", "stretch");
        pColumn->SetAttribute("margin", U8String(i == 0 ? "0,0,8,0" : "8,0,0,0"));
        pCharts->AddItem(pColumn);

        ui::Label* pLabel = AddLabel(pColumn, U8String(charts[i].title), "system_12",
                                     m_pal.textHint);
        pLabel->SetAttribute("height", "20");
        pLabel->SetAttribute("width", "stretch");

        activity::HistoryGraph* pGraph = new activity::HistoryGraph(this);
        pGraph->SetAttribute("height", "stretch");
        pGraph->SetAttribute("width", "stretch");
        pGraph->SetAttribute("margin", "0,4,0,0");
        pGraph->SetBkColor(m_pal.cardBg);
        pGraph->SetColours(charts[i].colour, WithAlpha(charts[i].colour, "55"),
                           WithAlpha(charts[i].colour, "00"), m_pal.grid);
        pColumn->AddItem(pGraph);
        *charts[i].target = pGraph;
    }

    for (int i = 0; i < kMaxNetRows; ++i) {
        ui::VBox* pBlock = new ui::VBox(this);
        pBlock->SetAttribute("height", "38");
        pBlock->SetAttribute("width", "stretch");
        pBlock->SetAttribute("margin", "0,6,0,0");
        pPanel->AddItem(pBlock);

        ui::HBox* pTop = new ui::HBox(this);
        pTop->SetAttribute("height", "20");
        pTop->SetAttribute("width", "stretch");
        pTop->SetAttribute("child_align", "vcenter");
        pBlock->AddItem(pTop);

        NetRow row;
        row.name = AddLabel(pTop, "—", "system_14", m_pal.textStrong);
        row.name->SetAttribute("width", "stretch");

        row.rate = AddLabel(pTop, "", "system_12", m_pal.textBody);
        row.rate->SetAttribute("width", "420");
        row.rate->SetAttribute("text_align", "right,vcenter");

        row.detail = AddLabel(pBlock, "", "system_12", m_pal.textHint);
        row.detail->SetAttribute("height", "18");
        row.detail->SetAttribute("width", "stretch");

        m_netRows.push_back(row);
    }

    m_pNetEmpty = AddLabel(pPanel, "没有活动的网络接口", "system_12",
                           m_pal.textHint);
    m_pNetEmpty->SetAttribute("height", "0");
    m_pNetEmpty->SetAttribute("width", "stretch");
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
            "负载 " + FormatDouble(m_cpu.load[0], "%.2f") + " " +
            FormatDouble(m_cpu.load[1], "%.2f") + " " +
            FormatDouble(m_cpu.load[2], "%.2f") + "   ·   运行 " +
            FormatUptime(m_cpu.uptime));
    }

    if (m_pCpuLegend.size() >= 4) {
        m_pCpuLegend[0]->SetText("用户 " + FormatDouble(m_cpu.user, "%.1f") + "%");
        m_pCpuLegend[1]->SetText("系统 " + FormatDouble(m_cpu.sys, "%.1f") + "%");
        m_pCpuLegend[2]->SetText("空闲 " + FormatDouble(m_cpu.idle, "%.1f") + "%");
        m_pCpuLegend[3]->SetText(
            m_cpu.temperature > 0.0
                ? "温度 " + FormatDouble(m_cpu.temperature, "%.1f") + " °C"
                : "温度 不可读");
    }

    for (size_t i = 0; i < m_pCoreBars.size(); ++i) {
        const double busy = i < m_cpu.coreBusy.size() ? m_cpu.coreBusy[i] : 0.0;
        m_pCoreBars[i]->SetFraction(static_cast<float>(busy / 100.0));
        if (i < m_pCoreReadings.size()) {
            m_pCoreReadings[i]->SetText(FormatDouble(busy, "%.0f") + "%");
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
            "已用 " + FormatBytes(m_mem.used) + " / " +
            FormatBytes(m_mem.total) + "  (" +
            FormatDouble(m_mem.usedPercent, "%.0f") + "%)");
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
            row.name->SetText("");
            row.detail->SetText("");
            row.bar->SetFraction(0.0f);
            continue;
        }
        const activity::DiskInfo& disk = m_disks[i];
        row.name->SetText(U8String(disk.mount.c_str()));
        row.detail->SetText(
            FormatBytes(disk.used) + " / " + FormatBytes(disk.total) +
            "   ·   可用 " + FormatBytes(disk.avail));
        row.bar->SetColours(m_pal.track,
                            disk.usedPercent >= 90.0 ? m_pal.diskFull : m_pal.diskFill);
        row.bar->SetFraction(static_cast<float>(disk.usedPercent / 100.0));
    }
    if (m_pDiskEmpty != nullptr) {
        m_pDiskEmpty->SetAttribute("height",
                                   m_disks.empty() ? "22" : "0");
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
        m_pNetHeadline->SetText("↓ " + FormatRate(down) + "   ↑ " +
                                FormatRate(up));
    }

    const size_t shown = (std::min)(m_netRows.size(), m_network.size());
    for (size_t i = 0; i < m_netRows.size(); ++i) {
        NetRow& row = m_netRows[i];
        if (i >= shown) {
            row.name->SetText("");
            row.rate->SetText("");
            row.detail->SetText("");
            continue;
        }
        const activity::NetInfo& info = m_network[i];
        row.name->SetText(U8String(info.name.c_str()) +
                          (info.up ? "  已连接" : "  未连接"));
        row.rate->SetText("↓ " + FormatRate(info.rxRate) + "   ↑ " +
                          FormatRate(info.txRate));
        U8String detail = "累计 收 " + FormatBytes(info.rxBytes) + " · 发 " +
                         FormatBytes(info.txBytes);
        for (const std::string& address : info.addresses) {
            detail += "   ·   " + U8String(address.c_str());
        }
        row.detail->SetText(detail);
    }
    if (m_pNetEmpty != nullptr) {
        m_pNetEmpty->SetAttribute("height",
                                  m_network.empty() ? "22" : "0");
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
            m_processRows[i].cells[c]->SetText("");
        }
    }
    if (m_pProcessEmpty != nullptr) {
        m_pProcessEmpty->SetAttribute("height",
                                      m_processes.empty() ? "22" : "0");
    }
}

void ActivityForm::FillProcessRow(size_t row, const activity::ProcessInfo& info)
{
    if (row >= m_processRows.size()) {
        return;
    }
    ProcessRow& cells = m_processRows[row];
    cells.cells[kColName]->SetText(U8String(info.name.c_str()));
    cells.cells[kColPid]->SetText(Num(info.pid));
    cells.cells[kColCpu]->SetText(FormatDouble(info.cpu, "%.1f"));
    cells.cells[kColTime]->SetText(FormatDuration(info.cpuSeconds));
    cells.cells[kColMemory]->SetText(FormatBytes(info.rss));
    cells.cells[kColThreads]->SetText(Num(info.threads));
    cells.cells[kColState]->SetText(U8String(activity::Sampler::StateText(info.state).c_str()));

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
                               " 个进程   ·   " + Num(threads) + " 个线程");
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
        U8String title = U8String(kColumnDefs[c].title);
        if (c == m_sortColumn) {
            title += m_sortAscending ? " ▲" : " ▼";
        }
        m_pColumnButtons[c]->SetText(title);
        m_pColumnButtons[c]->SetStateTextColor(
            ui::kControlStateNormal, c == m_sortColumn ? m_pal.textBody : m_pal.textHint);
    }
}

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------
U8String ActivityForm::FormatBytes(unsigned long long bytes)
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
    return Num(static_cast<long long>(bytes)) + " B";
}

U8String ActivityForm::FormatRate(double bytesPerSecond)
{
    if (bytesPerSecond >= 1024.0 * 1024.0) {
        return FormatDouble(bytesPerSecond / (1024.0 * 1024.0), "%.1f MB/s");
    }
    if (bytesPerSecond >= 1024.0) {
        return FormatDouble(bytesPerSecond / 1024.0, "%.1f KB/s");
    }
    return FormatDouble(bytesPerSecond, "%.0f B/s");
}

U8String ActivityForm::FormatDuration(double seconds)
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
    return U8String(buffer);
}

U8String ActivityForm::FormatUptime(long seconds)
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
    return U8String(buffer);
}
