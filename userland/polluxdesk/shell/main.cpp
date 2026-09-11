#include "dui/dui.h"
#include "PolluxOSForm.h"
#include "embedded_resources.inc"  // Build-time embedded resources
#include "dui/Utils/AppEntry.h"

/** App: FrameworkThread subclass that serves as the DUI_APP_ENTRY target.
 *  Launches the PolluxOS desktop shell (fullscreen dui window on the wlroots compositor).
 */
class App : public ui::FrameworkThread
{
public:
    App() : FrameworkThread(DUI_T("App"), ui::kThreadUI) {}

    void Run() { RunMessageLoop(); }

private:
    virtual void OnInit() override
    {
        // All resources (global.xml, images, fonts, language files) are embedded in
        // the executable and accessed directly from memory; no resource directory
        // is needed at runtime.
        ui::GlobalManager::Instance().Startup(
            ui::MemoryResParam(GetEmbeddedResourcesData(), GetEmbeddedResourcesSize()));

        PolluxOSForm* window = new PolluxOSForm();
        window->CreateWnd(nullptr, ui::WindowCreateParam(DUI_T("PolluxOS Desktop"), true));
        window->PostQuitMsgWhenClosed(true);
        window->ShowWindow(ui::kSW_SHOW_NORMAL);
    }

    virtual void OnCleanup() override
    {
        ui::GlobalManager::Instance().Shutdown();
    }
};

DUI_APP_ENTRY(App)
