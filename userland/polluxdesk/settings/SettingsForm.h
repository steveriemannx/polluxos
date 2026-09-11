#ifndef EXAMPLES_POLLUXDESK_SETTINGS_FORM_H_
#define EXAMPLES_POLLUXDESK_SETTINGS_FORM_H_

#include "dui/dui.h"

class SettingsForm : public ui::WindowImplBase
{
    typedef ui::WindowImplBase BaseClass;
public:
    SettingsForm();
    virtual ~SettingsForm() override;
    DString GetSkinFolder() override;
    DString GetSkinFile() override;
    void GetCreateWindowAttributes(ui::WindowCreateAttributes& attrs) override;
    void OnInitWindow() override;

private:
    void BuildUi();
    void ShowSection(int index);
    void BuildWallpaperPanel(ui::VBox* panel);
    void BuildNetworkPanel(ui::VBox* panel);
    void BuildWifiPanel(ui::VBox* panel);
    void BuildAboutPanel(ui::VBox* panel);
    void ApplyWallpaper(const char* name);
    void ApplyResolution(const char* value);
    void NotifyCompositor();

    ui::VBox*  m_pPanel = nullptr;      // settings content panel
    ui::Label* m_pStatus = nullptr;     // status/feedback label
    int        m_currentSection = 0;    // 0=wallpaper, 1=network, 2=wifi, 3=about
};

#endif // EXAMPLES_POLLUXDESK_SETTINGS_FORM_H_
