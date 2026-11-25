#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>

static int show_error(const wchar_t *message, int code)
{
    MessageBoxW(NULL, message, L"LightTouch iPad 4", MB_OK | MB_ICONERROR);
    return code;
}

static int is_file(const wchar_t *path)
{
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES &&
           !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR arguments,
                    int show_command)
{
    (void)instance;
    (void)previous;
    (void)show_command;

    const wchar_t *panel_option = L",retina=on";
    if (arguments && *arguments) {
        if (wcscmp(arguments, L"--low-resolution") == 0) {
            panel_option = L",retina=off";
        } else if (wcscmp(arguments, L"--retina") != 0) {
            return show_error(L"Launch normally for the scalable Retina display. "
                              L"Press F6 to rotate the iPad or F7 to put it to sleep or wake it. "
                              L"--low-resolution selects the experimental 1x panel.", 2);
        }
    }

    static wchar_t directory[32768];
    DWORD length = GetModuleFileNameW(NULL, directory, ARRAYSIZE(directory));
    if (!length || length >= ARRAYSIZE(directory)) {
        return show_error(L"Windows could not locate the LightTouch folder.", 1);
    }
    wchar_t *separator = wcsrchr(directory, L'\\');
    if (!separator) {
        return show_error(L"Windows could not locate the LightTouch folder.", 1);
    }
    separator[1] = L'\0';
    if (!SetCurrentDirectoryW(directory)) {
        return show_error(L"Windows could not open the LightTouch folder.", 1);
    }

    const wchar_t executable[] = L"runtime\\qemu-system-arm-display.exe";
    if (!is_file(executable) || !is_file(L"device\\normal.kboot") ||
        !is_file(L"device\\nand\\geometry.json") ||
        !is_file(L"device\\nand\\bus0-ce0.pages") ||
        !is_file(L"device\\nand\\bus1-ce0.pages")) {
        return show_error(L"The LightTouch runtime or the prepared virtual iPad is missing. "
                          L"Keep this executable with its runtime and device folders.", 2);
    }

    HANDLE lock = CreateFileW(L"device\\instance.lock", GENERIC_WRITE, 0, NULL,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (lock == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_SHARING_VIOLATION) {
            return show_error(L"This virtual iPad is already running.", 3);
        }
        return show_error(L"LightTouch could not open the virtual iPad for writing.", 3);
    }

    SECURITY_ATTRIBUTES inheritable = {sizeof(inheritable), NULL, TRUE};
    HANDLE log = CreateFileW(L"lighttouch.log", GENERIC_WRITE, FILE_SHARE_READ,
                             &inheritable, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               &inheritable, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) {
        if (log != INVALID_HANDLE_VALUE) {
            CloseHandle(log);
        }
        if (input != INVALID_HANDLE_VALUE) {
            CloseHandle(input);
        }
        CloseHandle(lock);
        return show_error(L"LightTouch could not open its launch log.", 4);
    }

    wchar_t command[1024];
    swprintf(command, ARRAYSIZE(command),
        L"qemu-system-arm-display.exe -name \"LightTouch iPad 4\" -L runtime/pc-bios "
        L"-M iPad-4-wifi,kboot=device/normal.kboot,ppn=on,nand=device/nand,"
        L"display-probe=on,quiet-boot=on,wake-on-boot=on%ls -m 1G "
        L"-display sdl,show-cursor=on -monitor none "
        L"-audio driver=none -no-reboot -serial file:device/boot.log", panel_option);
    STARTUPINFOW startup = {
        .cb = sizeof(startup),
        .dwFlags = STARTF_USESTDHANDLES,
        .hStdInput = input,
        .hStdOutput = log,
        .hStdError = log,
    };
    PROCESS_INFORMATION child;
    BOOL launched = CreateProcessW(executable, command, NULL, NULL, TRUE,
                                   CREATE_NO_WINDOW, NULL, directory, &startup, &child);
    DWORD error = launched ? ERROR_SUCCESS : GetLastError();
    CloseHandle(log);
    CloseHandle(input);
    if (!launched) {
        CloseHandle(lock);
        wchar_t message[256];
        swprintf(message, ARRAYSIZE(message),
                 L"Windows could not start the LightTouch emulator. Error %lu.", error);
        return show_error(message, 5);
    }
    CloseHandle(child.hThread);
    DWORD exit_code = 1;
    if (WaitForSingleObject(child.hProcess, INFINITE) == WAIT_OBJECT_0) {
        GetExitCodeProcess(child.hProcess, &exit_code);
    }
    CloseHandle(child.hProcess);
    CloseHandle(lock);
    if (exit_code) {
        wchar_t message[256];
        swprintf(message, ARRAYSIZE(message),
                 L"The emulator closed with error 0x%08lX. See lighttouch.log in this folder.",
                 exit_code);
        return show_error(message, 6);
    }
    return 0;
}
