﻿#include "pch.h"
#include <common/utils/window.h>
#include <common/utils/ProcessWaiter.h>
#include <common/utils/winapi_error.h>
#include <common/utils/logger_helper.h>
#include <keyboardmanager/common/KeyboardManagerConstants.h>
#include <keyboardmanager/KeyboardManagerEngineLibrary/KeyboardManager.h>
#include <keyboardmanager/KeyboardManagerEngineLibrary/trace.h>
#include <common/utils/UnhandledExceptionHandler_x64.h>

using namespace winrt;
using namespace Windows::Foundation;

const std::wstring instanceMutexName = L"Local\\PowerToys_KBMEngine_InstanceMutex";

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR lpCmdLine, int nCmdShow)
{
    init_apartment();
    LoggerHelpers::init_logger(KeyboardManagerConstants::ModuleName, L"Engine", LogSettings::keyboardManagerLoggerName);
    
    InitUnhandledExceptionHandler_x64();

    auto mutex = CreateMutex(nullptr, true, instanceMutexName.c_str());
    if (mutex == nullptr)
    {
        Logger::error(L"Failed to create mutex. {}", get_last_error_or_default(GetLastError()));
    }

    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        Logger::warn(L"KBM engine instance is already running");
        return 0;
    }

    Trace::RegisterProvider();

    std::wstring pid = std::wstring(lpCmdLine);
    if (!pid.empty())
    {
        auto mainThreadId = GetCurrentThreadId();
        ProcessWaiter::OnProcessTerminate(pid, [mainThreadId](int err) {
            if (err != ERROR_SUCCESS)
            {
                Logger::error(L"Failed to wait for parent process exit. {}", get_last_error_or_default(err));
            }
            else
            {
                Logger::trace(L"PowerToys runner exited.");
            }

            Logger::trace(L"Exiting KeyboardManager engine");
            PostThreadMessage(mainThreadId, WM_QUIT, 0, 0);
        });
    }

    auto kbm = KeyboardManager();
    kbm.StartLowlevelKeyboardHook();
    
    run_message_loop();
    
    kbm.StopLowlevelKeyboardHook();
    Trace::UnregisterProvider();
    
    return 0;
}
