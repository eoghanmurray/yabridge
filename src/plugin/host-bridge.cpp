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

#include "host-bridge.h"

#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/filesystem.hpp>
#include <boost/process/env.hpp>
#include <boost/process/io.hpp>
#include <boost/process/search_path.hpp>
#include <iostream>
#include <random>

#include "../common/communication.h"

// TODO: I should track down the VST2 SDK for clarification on some of the
//       implementation details, such as the use of intptr_t isntead of void*
//       here.

namespace bp = boost::process;
namespace fs = boost::filesystem;

/**
 * The name of the wine VST host binary.
 */
constexpr auto yabridge_wine_host_name = "yabridge-host.exe";

/**
 * Used for generating random identifiers.
 */
constexpr char alphanumeric_characters[] =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

fs::path find_vst_plugin();
fs::path find_wine_vst_host();
fs::path generate_endpoint_name();
bp::environment set_wineprefix();

intptr_t dispatch_proxy(AEffect*, int32_t, int32_t, intptr_t, void*, float);
void process_proxy(AEffect*, float**, float**, int32_t);
void process_replacing_proxy(AEffect*, float**, float**, int);
void setParameter_proxy(AEffect*, int32_t, float);
float getParameter_proxy(AEffect*, int32_t);

/**
 * Fetch the bridge instance stored in an unused pointer from a VST plugin. This
 * is sadly needed as a workaround to avoid using globals since we need free
 * function pointers to interface with the VST C API.
 */
HostBridge& get_bridge_instance(const AEffect& plugin) {
    return *static_cast<HostBridge*>(plugin.ptr3);
}

// TODO: When adding debug information, print both the path to the VST host and
//       the chosen wineprefix
HostBridge::HostBridge(audioMasterCallback host_callback)
    : io_context(),
      socket_endpoint(generate_endpoint_name().string()),
      socket_acceptor(io_context, socket_endpoint),
      host_vst_dispatch(io_context),
      vst_host_callback(io_context),
      vst_host_aeffect(io_context),
      host_callback_function(host_callback),
      vst_host(find_wine_vst_host(),
               // The Wine VST host needs to know which plugin to load and
               // which Unix domain socket to connect to
               find_vst_plugin(),
               socket_endpoint.path(),
               bp::env = set_wineprefix()) {
    // It's very important that these sockets are connected to in the same order
    // in the Wine VST host
    socket_acceptor.accept(host_vst_dispatch);
    socket_acceptor.accept(vst_host_callback);
    socket_acceptor.accept(vst_host_aeffect);

    // Set up all pointers for our `AEffect` struct. We will fill this with data
    // from the VST plugin loaded in Wine at the end of this constructor.
    plugin.ptr3 = this;
    plugin.dispatcher = dispatch_proxy;
    plugin.process = process_proxy;
    plugin.setParameter = setParameter_proxy;
    plugin.getParameter = getParameter_proxy;
    plugin.processReplacing = process_replacing_proxy;

    // TODO: Replace manual thread creation with an async_read loop
    // Start accepting host callbacks after we've set up our sockets and basic
    // `AEffect` struct.
    removeme = std::thread([&]() { return host_callback_loop(); });

    // Read the plugin's information from the Wine process. This can only be
    // done after we started accepting host callbacks as the plugin might do
    // this during initialization.
    // XXX: If the plugin has crashed then this read should fail instead of
    //      blocking indefinitely, check if this is the case
    plugin = read_object(vst_host_aeffect, plugin);
}

/**
 * Handle an event sent by the VST host. Most of these opcodes will be passed
 * through to the winelib VST host.
 */
intptr_t HostBridge::dispatch(AEffect* /*plugin*/,
                              int32_t opcode,
                              int32_t index,
                              intptr_t value,
                              void* data,
                              float option) {
    // Some events need some extra handling
    // TODO: Handle other things such as GUI itneraction
    switch (opcode) {
        case effClose:
            // TODO: Gracefully close the editor?
            // TODO: Check whether the sockets and the endpoint are closed
            //       correctly
            // XXX: Boost.Process will send SIGKILL to the process for us, is
            //      there a way to manually send a SIGTERM signal instead?

            // The VST API does not have an explicit function for releasing
            // resources, so we'll have to do it here. The actual plugin
            // instance gets freed by the host, or at least I think it does.
            delete this;

            return 0;
            break;
    }

    return send_event(host_vst_dispatch, opcode, index, value, data, option);
}

void HostBridge::process(AEffect* /*plugin*/,
                         float** /*inputs*/,
                         float** /*outputs*/,
                         int32_t /*sample_frames*/) {
    // TODO: Unimplmemented
}

void HostBridge::process_replacing(AEffect* /*plugin*/,
                                   float** /*inputs*/,
                                   float** /*outputs*/,
                                   int /*sample_frames*/) {
    // TODO: Unimplmemented
}

void HostBridge::set_parameter(AEffect* /*plugin*/,
                               int32_t /*index*/,
                               float /*value*/) {
    // TODO: Unimplmemented
}

float HostBridge::get_parameter(AEffect* /*plugin*/, int32_t /*index*/
) {
    // TODO: Unimplmemented
    return 0.0f;
}

/**
 * Finds the Wine VST hsot (named `yabridge-host.exe`). For this we will search
 * in two places:
 *
 *   1. Alongside libyabridge.so if the file got symlinked. This is useful
 *      when developing, as you can simply symlink the the libyabridge.so
 *      file in the build directory without having to install anything to
 *      /usr.
 *   2. In the regular search path.
 *
 * @return The a path to the VST host, if found.
 * @throw std::runtime_error If the Wine VST host could not be found.
 */
fs::path find_wine_vst_host() {
    fs::path host_path =
        fs::canonical(boost::dll::this_line_location()).remove_filename() /
        yabridge_wine_host_name;
    if (fs::exists(host_path)) {
        return host_path;
    }

    // Bosot will return an empty path if the file could not be found in the
    // search path
    const fs::path vst_host_path = bp::search_path(yabridge_wine_host_name);
    if (vst_host_path == "") {
        throw std::runtime_error("Could not locate '" +
                                 std::string(yabridge_wine_host_name) + "'");
    }

    return vst_host_path;
}

/**
 * Find the VST plugin .dll file that corresponds to this copy of
 * `libyabridge.so`. This should be the same as the name of this file but with a
 * `.dll` file extension instead of `.so`.
 *
 * @return The a path to the accompanying VST plugin .dll file.
 * @throw std::runtime_error If no matching .dll file could be found.
 */
fs::path find_vst_plugin() {
    fs::path plugin_path = boost::dll::this_line_location();
    plugin_path.replace_extension(".dll");

    // This function is used in the constructor's initializer list so we have to
    // throw when the path could not be found
    if (!fs::exists(plugin_path)) {
        throw std::runtime_error(
            "'" + plugin_path.string() +
            "' does not exist, make sure to rename 'libyabridge.so' to match a "
            "VST plugin .dll file.");
    }

    // Also resolve symlinks here, mostly for development purposes
    return fs::canonical(plugin_path);
}

/**
 * Generate a unique name for the Unix domain socket endpoint based on the VST
 * plugin's name. This will also generate the parent directory if it does not
 * yet exist since we're using this in the constructor's initializer list.
 *
 * @return A path to a not yet existing Unix domain socket endpoint.
 * @throw std::runtime_error If no matching .dll file could be found.
 */
fs::path generate_endpoint_name() {
    const auto plugin_name =
        find_vst_plugin().filename().replace_extension("").string();

    std::random_device random_device;
    std::mt19937 rng(random_device());
    fs::path candidate_endpoint;
    do {
        std::string random_id;
        std::sample(
            alphanumeric_characters,
            alphanumeric_characters + strlen(alphanumeric_characters) - 1,
            std::back_inserter(random_id), 8, rng);

        candidate_endpoint = fs::temp_directory_path() / "yabridge" /
                             (plugin_name + "-" + random_id + ".sock");
    } while (fs::exists(candidate_endpoint));

    // Ensure that the parent directory exists so the socket endpoint can be
    // created there
    fs::create_directories(candidate_endpoint.parent_path());

    // TODO: Should probably try creating the endpoint right here and catch any
    //       exceptions since this could technically result in a race condition
    //       when two instances of yabridge decide to use the same endpoint name
    //       at the same time

    return candidate_endpoint;
}

// TODO: Replace blocking loop with async readers or threads for all of the
//       sockets. Also extract this functionality somewhere since the host event
//       callback needs to do exactly the same thing.
void HostBridge::host_callback_loop() {
    while (true) {
        passthrough_event(vst_host_callback, &plugin, host_callback_function);
    }
}

/**
 * Locate the wineprefix and set the `WINEPREFIX` environment variable if found.
 * This way it's also possible to run .dll files outside of a wineprefix using
 * the user's default prefix.
 */
bp::environment set_wineprefix() {
    auto env(boost::this_process::environment());

    // Try to locate the wineprefix this .so file is located in by finding the
    // first parent directory that contains a directory named `dosdevices`
    fs::path wineprefix_path =
        boost::dll::this_line_location().remove_filename();
    while (wineprefix_path != "") {
        if (fs::is_directory(wineprefix_path / "dosdevices")) {
            env["WINEPREFIX"] = wineprefix_path.string();
            break;
        }

        wineprefix_path = wineprefix_path.parent_path();
    }

    return env;
}

// The below functions are proxy functions for the methods defined in
// `Bridge.cpp`

intptr_t dispatch_proxy(AEffect* plugin,
                        int32_t opcode,
                        int32_t index,
                        intptr_t value,
                        void* data,
                        float option) {
    return get_bridge_instance(*plugin).dispatch(plugin, opcode, index, value,
                                                 data, option);
}

void process_proxy(AEffect* plugin,
                   float** inputs,
                   float** outputs,
                   int32_t sample_frames) {
    return get_bridge_instance(*plugin).process(plugin, inputs, outputs,
                                                sample_frames);
}

void process_replacing_proxy(AEffect* plugin,
                             float** inputs,
                             float** outputs,
                             int sample_frames) {
    return get_bridge_instance(*plugin).process_replacing(
        plugin, inputs, outputs, sample_frames);
}

void setParameter_proxy(AEffect* plugin, int32_t index, float value) {
    return get_bridge_instance(*plugin).set_parameter(plugin, index, value);
}

float getParameter_proxy(AEffect* plugin, int32_t index) {
    return get_bridge_instance(*plugin).get_parameter(plugin, index);
}