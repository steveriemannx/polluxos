#include "SettingsForm.h"
#include "embedded_resources.inc"
#include "dui/Utils/AppEntry.h"

class App : public ui::FrameworkThread
{
public:
    App() : FrameworkThread("App", ui::kThreadUI) {}
    void Run() { RunMessageLoop(); }

private:
    void OnInit() override
    {
        ui::GlobalManager::Instance().Startup(
            ui::MemoryResParam(EmbeddedResources()));
        SettingsForm* window = new SettingsForm();
        window->CreateWnd(nullptr, ui::WindowCreateParam("PolluxOS Settings", true));
        window->PostQuitMsgWhenClosed(true);
        window->ShowWindow(ui::kSW_SHOW_NORMAL);
    }
    void OnCleanup() override
    {
        ui::GlobalManager::Instance().Shutdown();
    }
};

DUI_APP_ENTRY(App)
