#include "LaunchPadForm.h"
#include "PolluxTheme.h"
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
            PolluxResParam());
        LaunchPadForm* window = new LaunchPadForm();
        window->CreateWnd(nullptr,
            ui::WindowCreateParam("PolluxOS Launchpad", true));
        window->PostQuitMsgWhenClosed(true);
        window->SetLaunchHandler(nullptr);
        window->RefreshAndShow();
    }

    void OnCleanup() override
    {
        ui::GlobalManager::Instance().Shutdown();
    }
};

DUI_APP_ENTRY(App)
