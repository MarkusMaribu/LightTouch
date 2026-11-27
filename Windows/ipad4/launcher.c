#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static const wchar_t executable[] = L"runtime\\qemu-system-arm-display.exe";

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

static BOOL run_engine(wchar_t *command, STARTUPINFOW *startup, DWORD *exit_code)
{
    PROCESS_INFORMATION child;
    if (!CreateProcessW(executable, command, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                        NULL, NULL, startup, &child)) {
        return FALSE;
    }
    CloseHandle(child.hThread);
    BOOL done = WaitForSingleObject(child.hProcess, INFINITE) == WAIT_OBJECT_0 &&
                GetExitCodeProcess(child.hProcess, exit_code);
    CloseHandle(child.hProcess);
    return done;
}

static BOOL audio_upgrade_verified(void)
{
    FILE *log = _wfopen(L"device\\repair-audio.log", L"rb");
    if (!log) {
        return FALSE;
    }
    char line[1024];
    unsigned successes = 0;
    BOOL failed = FALSE, shutdown = FALSE;
    while (fgets(line, sizeof(line), log)) {
        char *start = line + strspn(line, "\r");
        start[strcspn(start, "\r\n")] = 0;
        if (strcmp(start, "P101AUDIO OK restored") == 0 ||
            strcmp(start, "P101AUDIO OK already-enabled") == 0) {
            successes++;
        } else if (strcmp(start, "P101AUDIO FAILED") == 0) {
            failed = TRUE;
        } else if (successes && strcmp(start, "pmu go stdby") == 0) {
            shutdown = TRUE;
        }
    }
    BOOL verified = successes == 1 && !failed && shutdown && !ferror(log);
    fclose(log);
    return verified;
}

static BOOL enable_stock_audio(STARTUPINFOW *startup)
{
    const wchar_t marker[] = L"device\\nand\\audio-hal-v1.ok";
    if (is_file(marker) || !is_file(L"runtime\\repair-audio.kboot")) {
        return TRUE;
    }
    /* Only packages upgrading an older prepared iPad include this boot image. */
    wchar_t command[] =
        L"qemu-system-arm-display.exe -L runtime/pc-bios "
        L"-M iPad-4-wifi,kboot=runtime/repair-audio.kboot,ppn=on,nand=device/nand "
        L"-m 1G -display none -monitor none -audio driver=none "
        L"-no-reboot -serial file:device/repair-audio.log";
    DWORD status;
    FILE *log = _wfopen(L"device\\repair-audio.log", L"wb");
    if (!log || fclose(log) != 0 || !run_engine(command, startup, &status) ||
        status != 0 || !audio_upgrade_verified()) {
        return FALSE;
    }
    HANDLE file = CreateFileW(marker, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return FALSE;
    }
    return CloseHandle(file);
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
                              L"Press F8 for Siri. --low-resolution selects the experimental 1x panel.", 2);
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

    if (!is_file(executable) || !is_file(L"device\\normal.kboot") ||
        !is_file(L"device\\nand\\geometry.json") ||
        !is_file(L"device\\nand\\bus0-ce0.pages") ||
        !is_file(L"device\\nand\\bus1-ce0.pages")) {
        return show_error(L"The LightTouch runtime or the prepared virtual iPad is missing. "
                          L"Keep this executable with its runtime and device folders.", 2);
    }

    SECURITY_ATTRIBUTES inheritable = {sizeof(inheritable), NULL, TRUE};
    HANDLE lock = CreateFileW(L"device\\instance.lock", GENERIC_WRITE, 0, &inheritable,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (lock == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_SHARING_VIOLATION) {
            return show_error(L"This virtual iPad is already running.", 3);
        }
        return show_error(L"LightTouch could not open the virtual iPad for writing.", 3);
    }

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
        L"display-probe=on,quiet-boot=on,wake-on-boot=on,"
        L"audio-ports-probe=on,ae2-probe=on,tristar-probe=on%ls -m 1G "
        L"-display sdl,show-cursor=on -monitor none "
        L"-audio driver=sdl -no-reboot -serial file:device/boot.log", panel_option);
    STARTUPINFOW startup = {
        .cb = sizeof(startup),
        .dwFlags = STARTF_USESTDHANDLES,
        .hStdInput = input,
        .hStdOutput = log,
        .hStdError = log,
    };
    if (!enable_stock_audio(&startup)) {
        CloseHandle(input);
        CloseHandle(log);
        CloseHandle(lock);
        return show_error(L"The iPad could not verify its stock audio driver. "
                          L"See device\\repair-audio.log and lighttouch.log.", 7);
    }
    DWORD exit_code = 1;
    BOOL launched = run_engine(command, &startup, &exit_code);
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
