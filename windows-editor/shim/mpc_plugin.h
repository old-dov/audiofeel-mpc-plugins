// Shim in front of the SDK's framework/mpc_plugin.h for the Windows editor build only.
// It includes the real header, then redefines MPC_PLUGIN_ENTRY so that the plugin's own source
// (unchanged) ends up with an effect that has an editor. Nothing here touches the MPC builds.
#pragma once

#include_next "mpc_plugin.h"

#if defined(_WIN32) && defined(AUDIOFEEL_EDITOR)

#include "af_editor_win.h"
#include "af_layout.h"   // generated from the plugin's ui.json by gen_layout.py

#undef MPC_PLUGIN_ENTRY
#define MPC_PLUGIN_ENTRY(PluginClass)                                                        \
    extern "C" MPC_EXPORT AEffect* VSTPluginMain(audioMasterCallback host)                   \
    {                                                                                        \
        if (host != nullptr && host(nullptr, audioMasterVersion, 0, 0, nullptr, 0.0f) == 0)  \
            return nullptr;                                                                  \
        AEffect* effect = mpc::detail::createEffect(new PluginClass(), host);                \
        afgui::attach(effect, af_layout());                                                  \
        return effect;                                                                       \
    }

#endif
