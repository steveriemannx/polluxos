#include "LaunchPadForm.h"
#include "embedded_resources.inc"
#include "dui/Utils/AppEntry.h"

class App : public ui::FrameworkThread
{
public:
    App() : FrameworkThread(_T("App"), ui::kThreadUI) {}
    void Run() { RunMessageLoop(); }

private:
    void OnInit() override
    {
        ui::GlobalManager::Instance().Startup(
            ui::MemoryResParam(GetEmbeddedResourcesData(), GetEmbeddedResourcesSize()));
        LaunchPadForm* window = new LaunchPadForm();
        window->CreateWnd(nullptr,
            ui::WindowCreateParam(_T("PolluxOS Launchpad"), true));
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
