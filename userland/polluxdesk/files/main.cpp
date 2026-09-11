#include "FilesForm.h"
#include "embedded_resources.inc"  // Build-time embedded resources
#include "dui/Utils/AppEntry.h"

/** App: FrameworkThread subclass that serves as the DUI_APP_ENTRY target.
 *  Launches the PolluxOS file manager (a regular floating app window on the
 *  wlroots compositor; the compositor draws its titlebar and traffic lights).
 */
class App : public ui::FrameworkThread
{
public:
    App() : FrameworkThread(DUI_T("App"), ui::kThreadUI) {}

    void Run() { RunMessageLoop(); }

private:
    virtual void OnInit() override
    {
        // All resources (global.xml, images, fonts, language files) are
        // embedded in the executable; no resource directory is needed.
        ui::GlobalManager::Instance().Startup(
            ui::MemoryResParam(GetEmbeddedResourcesData(), GetEmbeddedResourcesSize()));

        FilesForm* window = new FilesForm();
        window->CreateWnd(nullptr, ui::WindowCreateParam(DUI_T("PolluxOS Files"), true));
        // Clicking the compositor's red traffic light sends an xdg close:
        // without this the window is destroyed but the process lingers
        // invisibly (the "traffic lights do not work" symptom).
        window->PostQuitMsgWhenClosed(true);
        window->ShowWindow(ui::kSW_SHOW_NORMAL);
    }

    virtual void OnCleanup() override
    {
        ui::GlobalManager::Instance().Shutdown();
    }
};

DUI_APP_ENTRY(App)
