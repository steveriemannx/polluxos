#pragma once

#include "dui/dui.h"
#include "embedded_resources.inc"

/** Resource parameters for the PolluxOS desktop components.
 *
 *  PolluxOS has its own dui theme: every component embeds themes/polluxos (see
 *  EMBED_RES_PATHS in the component CMakeLists) and selects it here. The platform
 *  theme -- themes/freebsd on FreeBSD, which is what GetDefaultThemePath() picks --
 *  is deliberately not packaged, so a resource the polluxos theme does not carry
 *  can never be silently satisfied from another theme.
 */
inline ui::MemoryResParam PolluxResParam()
{
    ui::MemoryResParam resParam(EmbeddedResources());
    resParam.themePath = ui::FilePath("themes/polluxos");
    return resParam;
}
