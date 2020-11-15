// yabridge: a Wine VST bridge
// Copyright (C) 2020  Robbert van der Helm
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include <vestige/aeffectx.h>

#include <iostream>
#include <memory>

#include "../common/logging.h"
#include "plugin-bridge.h"

#define VST_EXPORT __attribute__((visibility("default")))

// The main entry point for VST plugins should be called `VSTPluginMain``. The
// other one exist for legacy reasons since some old hosts might still use them.
// There's also another possible legacy entry point just called `main`, but GCC
// will refuse to compile a function called `main` that's not a regular C++ main
// function

/**
 * The main VST plugin entry point. We first set up a bridge that connects to a
 * Wine process that hosts the Windows VST plugin. We then create and return a
 * VST plugin struct that acts as a passthrough to the bridge.
 *
 * To keep this somewhat contained this is the only place where we're doing
 * manual memory management. Clean up is done when we receive the `effClose`
 * opcode from the VST host (i.e. opcode 1).`
 */
extern "C" VST_EXPORT AEffect* VSTPluginMain(
    audioMasterCallback host_callback) {
    // HACK: Workaround for a bug in Bitwig Studio 3.3 beta 4. `environ` is a
    //       null pointer in beta 4, which breaks Boost.Process and any other
    //       environment access through `environ`. To work around this for this
    //       beta, we'll just overwrite `environ` with the contents of
    //       `/proc/self/environ`.
    //
    //       This should **not** be committed to the master branch.
    if (!environ) {
        auto* reconstructed_environment = new std::vector<char*>{};

        std::ifstream environ_file("/proc/self/environ", std::ifstream::in);
        for (std::string variable;
             std::getline(environ_file, variable, '\0');) {
            // Memory leaks galore!
            reconstructed_environment->push_back(
                const_cast<char*>((new std::string(variable))->c_str()));
        }

        // `environ` should end with a trailing null pointer
        reconstructed_environment->push_back(nullptr);
        environ = reconstructed_environment->data();
        __environ = reconstructed_environment->data();
    }

    try {
        // This is the only place where we have to use manual memory management.
        // The bridge's destructor is called when the `effClose` opcode is
        // received.
        PluginBridge* bridge = new PluginBridge(host_callback);

        return &bridge->plugin;
    } catch (const std::exception& error) {
        Logger logger = Logger::create_from_environment();
        logger.log("Error during initialization:");
        logger.log(error.what());

        return nullptr;
    }
}

extern "C" VST_EXPORT AEffect* main_plugin(audioMasterCallback audioMaster) {
    return VSTPluginMain(audioMaster);
}
