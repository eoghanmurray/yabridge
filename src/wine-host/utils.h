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

#pragma once

#include "boost-fix.h"

#include <memory>
#include <optional>

#define NOMINMAX
#define NOSERVICE
#define NOMCX
#define NOIMM
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <boost/asio/io_context.hpp>
#include <function2/function2.hpp>

/**
 * The delay between calls to the event loop so we can keep a nice 60 fps. We
 * could bump this up to the monitor's refresh rate, but I'm afraid that it will
 * start to noticeably take up resources in plugin groups.
 */
constexpr std::chrono::duration event_loop_interval =
    std::chrono::milliseconds(1000) / 60;

/**
 * A wrapper around `boost::asio::io_context()` to serve as the application's
 * main IO context. A single instance is shared for all plugins in a plugin
 * group so that several important events can be handled on the main thread,
 * which can be required because in the Win32 model all GUI related operations
 * have to be handled from the same thread. This will be run from the
 * application's main thread.
 */
class MainContext {
   public:
    MainContext();

    /**
     * Run the IO context. This rest of this class assumes that this is only
     * done from a single thread.
     */
    void run();

    /**
     * Drop all future work from the IO context. This does not necessarily mean
     * that the thread that called `main_context.run()` immediatly returns.
     */
    void stop();

    /**
     * Start a timer to handle events every `event_loop_interval` milliseconds.
     *
     * @param handler The function that should be executed in the IO context
     *   when the timer ticks. This should be a function that handles both the
     *   X11 events and the Win32 message loop.
     */
    template <typename F>
    void async_handle_events(F handler) {
        // Try to keep a steady framerate, but add in delays to let other events
        // get handled if the GUI message handling somehow takes very long.
        events_timer.expires_at(std::max(
            events_timer.expiry() + event_loop_interval,
            std::chrono::steady_clock::now() + std::chrono::milliseconds(5)));
        events_timer.async_wait(
            [&, handler](const boost::system::error_code& error) {
                if (error.failed()) {
                    return;
                }

                handler();
                async_handle_events(handler);
            });
    }

    /**
     * The raw IO context. Can and should be used directly for everything that's
     * not the event handling loop.
     */
    boost::asio::io_context context;

   private:
    /**
     * The timer used to periodically handle X11 events and Win32 messages.
     */
    boost::asio::steady_timer events_timer;
};

/**
 * A proxy function that calls `Win32Thread::entry_point` since `CreateThread()`
 * is not usable with lambdas directly. Calling the passed function will invoke
 * the lambda with the arguments passed during `Win32Thread`'s constructor. This
 * function deallocates the function after it's finished executing.
 *
 * We can't store the function pointer in the `Win32Thread` object because
 * moving a `Win32Thread` object would then cause issues.
 *
 * @param entry_point A `fu2::unique_function<void()>*` pointer to a function
 *   pointer, great.
 */
uint32_t WINAPI
win32_thread_trampoline(fu2::unique_function<void()>* entry_point);

/**
 * A simple RAII wrapper around the Win32 thread API that imitates
 * `std::jthread`, including implicit joining (or waiting, since this is Win32)
 * on destruction.
 *
 * `std::thread` uses pthreads directly in Winelib (since this is technically a
 * regular Linux application). This means that when using
 * `std::thread`/`std::jthread` directly, some thread local information that
 * `CreateThread()` would normally set does not get initialized. This could then
 * lead to memory errors. This wrapper aims to be equivalent to `std::jthread`,
 * but using the Win32 API instead.
 *
 * @note This should be used instead of `std::thread` or `std::jthread` whenever
 *   the thread directly calls third party library code, i.e. `LoadLibrary()`,
 *   `FreeLibrary()`, the plugin's entry point, or any of the `AEffect::*()`
 *   functions.
 */
class Win32Thread {
   public:
    /**
     * Constructor that does not start any thread yet.
     */
    Win32Thread();

    /**
     * Constructor that immediately starts running the thread. This works
     * equivalently to `std::jthread`.
     *
     * @param entry_point The thread entry point that should be run.
     * @param parameter The parameter passed to the entry point function.
     */
    template <typename Function, typename... Args>
    Win32Thread(Function&& f, Args&&... args)
        : handle(
              CreateThread(
                  nullptr,
                  0,
                  reinterpret_cast<LPTHREAD_START_ROUTINE>(
                      win32_thread_trampoline),
                  // `std::function` does not support functions with move
                  // captures the function has to be copy-constructable.
                  // Function2's unique_function lets us capture and move our
                  // arguments to the lambda so we don't end up with dangling
                  // references.
                  new fu2::unique_function<void()>(
                      [f = std::move(f), ... args = std::move(args)]() mutable {
                          f(std::move(args)...);
                      }),
                  0,
                  nullptr),
              CloseHandle) {}

    /**
     * Join (or wait on, since this is WIn32) the thread on shutdown, just like
     * `std::jthread` does.
     */
    ~Win32Thread();

    Win32Thread(const Win32Thread&) = delete;
    Win32Thread& operator=(const Win32Thread&) = delete;

    Win32Thread(Win32Thread&&);
    Win32Thread& operator=(Win32Thread&&);

   private:
    /**
     * The handle for the thread that is running, will be a null pointer if this
     * class was constructed with the default constructor.
     */
    std::unique_ptr<std::remove_pointer_t<HANDLE>, decltype(&CloseHandle)>
        handle;
};

/**
 * A simple RAII wrapper around `SetTimer`. Does not support timer procs since
 * we don't use them.
 */
class Win32Timer {
   public:
    Win32Timer(HWND window_handle, size_t timer_id, unsigned int interval_ms);
    ~Win32Timer();

    Win32Timer(const Win32Timer&) = delete;
    Win32Timer& operator=(const Win32Timer&) = delete;

    Win32Timer(Win32Timer&&);
    Win32Timer& operator=(Win32Timer&&);

   private:
    HWND window_handle;
    std::optional<size_t> timer_id;
};
