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

#include "utils.h"

#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/process/io.hpp>
#include <boost/process/pipe.hpp>
#include <boost/process/search_path.hpp>
#include <boost/process/system.hpp>
#include <fstream>
#include <random>
#include <sstream>

// Generated inside of build directory
#include <src/common/config/config.h>

namespace bp = boost::process;
namespace fs = boost::filesystem;

/**
 * Used for generating random identifiers.
 */
constexpr char alphanumeric_characters[] =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

std::string create_logger_prefix(const fs::path& socket_path) {
    // Use the socket filename as the logger prefix, but strip the `yabridge-`
    // part since that's redundant
    std::string socket_name =
        socket_path.filename().replace_extension().string();
    const std::string socket_prefix("yabridge-");
    assert(socket_name.find(socket_prefix) == 0);
    socket_name = socket_name.substr(socket_prefix.size());

    std::ostringstream prefix;
    prefix << "[" << socket_name << "] ";

    return prefix.str();
}

std::optional<fs::path> find_wineprefix() {
    // Try to locate the Wine prefix the plugin's .dll file is located in by
    // finding the first parent directory that contains a directory named
    // `dosdevices`
    fs::path wineprefix_path = find_vst_plugin();
    while (wineprefix_path != "") {
        if (fs::is_directory(wineprefix_path / "dosdevices")) {
            return wineprefix_path;
        }

        wineprefix_path = wineprefix_path.parent_path();
    }

    return std::nullopt;
}

PluginArchitecture find_vst_architecture(fs::path plugin_path) {
    std::ifstream file(plugin_path, std::ifstream::binary | std::ifstream::in);

    // The linker will place the offset where the PE signature is placed at the
    // end of the MS-DOS stub, at offset 0x3c
    uint32_t pe_signature_offset;
    file.seekg(0x3c);
    file.read(reinterpret_cast<char*>(&pe_signature_offset),
              sizeof(pe_signature_offset));

    // The PE32 signature will be followed by a magic number that indicates the
    // target architecture of the binary
    uint32_t pe_signature;
    uint16_t machine_type;
    file.seekg(pe_signature_offset);
    file.read(reinterpret_cast<char*>(&pe_signature), sizeof(pe_signature));
    file.read(reinterpret_cast<char*>(&machine_type), sizeof(machine_type));

    constexpr char expected_pe_signature[4] = {'P', 'E', '\0', '\0'};
    if (pe_signature !=
        *reinterpret_cast<const uint32_t*>(expected_pe_signature)) {
        throw std::runtime_error("'" + plugin_path.string() +
                                 "' is not a valid .dll file");
    }

    // These constants are specified in
    // https://docs.microsoft.com/en-us/windows/win32/debug/pe-format#machine-types
    switch (machine_type) {
        case 0x014c:  // IMAGE_FILE_MACHINE_I386
            return PluginArchitecture::vst_32;
            break;
        case 0x8664:  // IMAGE_FILE_MACHINE_AMD64
        case 0x0000:  // IMAGE_FILE_MACHINE_UNKNOWN
            return PluginArchitecture::vst_64;
            break;
    }

    // When compiled without optimizations, GCC 9.3 will warn that the function
    // does not return if we put this in a `default:` case instead.
    std::ostringstream error_msg;
    error_msg << "'" << plugin_path
              << "' is neither a x86 nor a x86_64 PE32 file. Actual "
                 "architecture: 0x"
              << std::hex << machine_type;
    throw std::runtime_error(error_msg.str());
}

fs::path find_vst_host(PluginArchitecture plugin_arch) {
    auto host_name = yabridge_wine_host_name;
    if (plugin_arch == PluginArchitecture::vst_32) {
        host_name = yabridge_wine_host_name_32bit;
    }

    fs::path host_path =
        fs::canonical(get_this_file_location()).remove_filename() / host_name;
    if (fs::exists(host_path)) {
        return host_path;
    }

    // Bosot will return an empty path if the file could not be found in the
    // search path
    const fs::path vst_host_path = bp::search_path(host_name);
    if (vst_host_path == "") {
        throw std::runtime_error("Could not locate '" + std::string(host_name) +
                                 "'");
    }

    return vst_host_path;
}

fs::path find_vst_plugin() {
    const fs::path this_plugin_path =
        "/" / fs::path("/" + get_this_file_location().string());

    fs::path plugin_path(this_plugin_path);
    plugin_path.replace_extension(".dll");
    if (fs::exists(plugin_path)) {
        // Also resolve symlinks here, to support symlinked .dll files
        return fs::canonical(plugin_path);
    }

    // In case this files does not exist and our `.so` file is a symlink, we'll
    // also repeat this check after resolving that symlink to support links to
    // copies of `libyabridge.so` as described in issue #3
    fs::path alternative_plugin_path = fs::canonical(this_plugin_path);
    alternative_plugin_path.replace_extension(".dll");
    if (fs::exists(alternative_plugin_path)) {
        return fs::canonical(alternative_plugin_path);
    }

    // This function is used in the constructor's initializer list so we have to
    // throw when the path could not be found
    throw std::runtime_error("'" + plugin_path.string() +
                             "' does not exist, make sure to rename "
                             "'libyabridge.so' to match a "
                             "VST plugin .dll file.");
}

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

        // We'll get rid of the file descriptors immediately after accepting the
        // sockets, so putting them inside of a subdirectory would only leave
        // behind an empty directory
        std::ostringstream socket_name;
        socket_name << "yabridge-" << plugin_name << "-" << random_id
                    << ".sock";

        candidate_endpoint = fs::temp_directory_path() / socket_name.str();
    } while (fs::exists(candidate_endpoint));

    // TODO: Should probably try creating the endpoint right here and catch any
    //       exceptions since this could technically result in a race condition
    //       when two instances of yabridge decide to use the same endpoint name
    //       at the same time

    return candidate_endpoint;
}

fs::path get_this_file_location() {
    // HACK: Not sure why, but `boost::dll::this_line_location()` returns a path
    //       starting with a double slash on some systems. I've seen this happen
    //       on both Ubuntu 18.04 and 20.04, but not on Arch based distros.
    //       Under Linux a path starting with two slashes is treated the same as
    //       a path starting with only a single slash, but Wine will refuse to
    //       load any files when the path starts with two slashes. Prepending
    //       `/` to a pad coerces theses two slashes into a single slash.
    return "/" / boost::dll::this_line_location();
}

std::string get_wine_version() {
    // The '*.exe' scripts generated by winegcc allow you to override the binary
    // used to run Wine, so will will respect this as well
    std::string wine_command = "wine";

    bp::native_environment env = boost::this_process::environment();
    if (!env["WINELOADER"].empty()) {
        wine_command = env.get("WINELOADER");
    }

    bp::ipstream output;
    try {
        const fs::path wine_path = bp::search_path(wine_command);
        bp::system(wine_path, "--version", bp::std_out = output);
    } catch (const std::system_error&) {
        return "<NOT FOUND>";
    }

    // `wine --version` might contain additional output in certain custom Wine
    // builds, so we only want to look at the first line
    std::string version_string;
    std::getline(output, version_string);

    // Strip the `wine-` prefix from the output, could potentially be absent in
    // custom Wine builds
    const std::string version_prefix("wine-");
    if (version_string.find(version_prefix) == 0) {
        version_string = version_string.substr(version_prefix.size());
    }

    return version_string;
}

bp::environment set_wineprefix() {
    bp::native_environment env = boost::this_process::environment();

    // Allow the wine prefix to be overridden manually
    if (!env["WINEPREFIX"].empty()) {
        return env;
    }

    const auto wineprefix_path = find_wineprefix();
    if (wineprefix_path.has_value()) {
        env["WINEPREFIX"] = wineprefix_path->string();
    }

    return env;
}