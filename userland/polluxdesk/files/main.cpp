#include "FilesForm.h"
#include "embedded_resources.inc"  // Build-time embedded resources
#include "dui/Utils/AppEntry.h"

/** App: FrameworkThread subclass that serves as the DUI_APP_ENTRY_ARGS
 *  target.  Launches the PolluxOS file manager (a regular floating app window
 *  on the wlroots compositor; the compositor draws its titlebar and traffic
 *  lights).
 */
class App : public ui::FrameworkThread
{
public:
    App() : FrameworkThread(DUI_T("App"), ui::kThreadUI) {}

    /** The entry macro builds the process entry point around one instance. */
    static App& Instance()
    {
        static App instance;
        return instance;
    }

    /** argv[1], when present, is the directory to open.  The dock uses it to
     *  show the trash, and a terminal can point the browser anywhere. */
    int Run(int argc, char** argv)
    {
        if (argc > 1 && argv[1] != nullptr && argv[1][0] != '\0') {
            m_startDir = DString(argv[1]);
        }
        RunMessageLoop();
        return 0;
    }

private:
    virtual void OnInit() override
    {
        // All resources (global.xml, images, fonts, language files) are
        // embedded in the executable; no resource directory is needed.
        ui::GlobalManager::Instance().Startup(
            ui::MemoryResParam(GetEmbeddedResourcesData(), GetEmbeddedResourcesSize()));

        FilesForm* window = new FilesForm();
        window->SetStartDir(m_startDir);
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

    DString m_startDir;   // empty means "start in the home directory"
};

DUI_APP_ENTRY_ARGS(App)
