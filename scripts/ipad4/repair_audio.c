/* Restore the one bundle moved by ipad4.py prepare --silent-audio. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define PLUGINS "Library/Audio/Plug-Ins"
#define OFF PLUGINS "/VirtualAudio.plugin"
#define ON PLUGINS "/HAL/VirtualAudio.plugin"
#define VERSION "System/Library/CoreServices/SystemVersion.plist"

struct fingerprint {
    size_t size;
    uint32_t fnv;
};

enum audio_state { FAILED, ENABLED, RESTORED };

static int matches(FILE *log, const char *path, struct fingerprint expected)
{
    unsigned char buf[4096];
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(log, "P101AUDIO cannot read %s: %s\n", path, strerror(errno));
        return 0;
    }
    uint32_t fnv = 2166136261u;
    size_t size = 0;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), file)) != 0) {
        size += n;
        if (size > expected.size) break;
        for (size_t i = 0; i < n; i++) fnv = (fnv ^ buf[i]) * 16777619u;
    }
    int ok = !ferror(file) && size == expected.size && fnv == expected.fnv;
    fclose(file);
    if (!ok) fprintf(log, "P101AUDIO unexpected contents of %s\n", path);
    return ok;
}

static int bundle_present(FILE *log, const char *path)
{
    struct stat st;
#ifdef _WIN32   /* Native fixture on Windows. The iPad always uses lstat. */
    int rc = stat(path, &st);
#else
    int rc = lstat(path, &st);
#endif
    if (rc == 0 && S_ISDIR(st.st_mode)) return 1;
    if (rc != 0 && errno == ENOENT) return 0;
    fprintf(log, "P101AUDIO cannot inspect bundle directory %s\n", path);
    return -1;
}

static enum audio_state repair(FILE *log, int allow_move, const struct fingerprint files[3])
{
    if (!matches(log, VERSION, files[0])) return FAILED;
    int on = bundle_present(log, ON);
    int off = bundle_present(log, OFF);
    if (on < 0 || off < 0 || on == off) {
        fprintf(log, "P101AUDIO expected exactly one VirtualAudio bundle\n");
        return FAILED;
    }
    if (!matches(log, on ? ON "/Info.plist" : OFF "/Info.plist", files[1]) ||
        !matches(log, on ? ON "/VirtualAudio" : OFF "/VirtualAudio", files[2])) return FAILED;
    if (on) return ENABLED;
    if (!allow_move) {
        fprintf(log, "P101AUDIO bundle is still outside HAL\n");
        return FAILED;
    }
    if (rename(OFF, ON) != 0) {
        fprintf(log, "P101AUDIO failed to restore bundle: %s\n", strerror(errno));
        return FAILED;
    }
    return RESTORED;
}

#ifndef P101_AUDIO_FIXTURE
#include <sys/wait.h>
#include <unistd.h>

extern int reboot(int);
extern int unmount(const char *, int);

static int mount_system(FILE *log, int readonly)
{
    pid_t pid = vfork();
    if (pid == 0) {
        if (readonly) {
            execl("/sbin/mount_hfs", "mount_hfs", "-o", "rdonly",
                  "/dev/disk0s1", "/mnt1", (char *)0);
        } else {
            execl("/sbin/mount_hfs", "mount_hfs", "/dev/disk0s1", "/mnt1", (char *)0);
        }
        _exit(127);
    }
    int status;
    if (pid > 0) {
        pid_t done;
        do { done = waitpid(pid, &status, 0); } while (done < 0 && errno == EINTR);
        if (done == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0) return 1;
    }
    fprintf(log, "P101AUDIO failed to mount System%s\n", readonly ? " read-only" : "");
    return 0;
}

static enum audio_state mount_and_check(FILE *log, int verify)
{
    /* Length and FNV-1a of the stock 10B329 SystemVersion, plug-in plist and driver. */
    static const struct fingerprint stock[] = {
        {416, 0x0edf3aed}, {913, 0x2f9d6275}, {2382752, 0xf8d7e888}
    };
    if (!mount_system(log, verify)) return FAILED;
    enum audio_state state = FAILED;
    if (chdir("/mnt1") == 0) state = repair(log, !verify, stock);
    if (state == RESTORED) sync();
    if (chdir("/") != 0 || unmount("/mnt1", 0) != 0) {
        fprintf(log, "P101AUDIO failed to unmount System: %s\n", strerror(errno));
        return FAILED;
    }
    return state;
}

int main(void)
{
    FILE *log = fopen("/dev/console", "w");
    if (!log) log = stderr;
    for (int left = 60; left && access("/dev/disk0s1", F_OK); left--) sleep(1);
    mkdir("/mnt1", 0755);
    enum audio_state result = mount_and_check(log, 0);
    if (result != FAILED && mount_and_check(log, 1) == ENABLED) {
        fprintf(log, "P101AUDIO OK %s\n", result == RESTORED ? "restored" : "already-enabled");
    } else {
        fprintf(log, "P101AUDIO FAILED\n");
    }
    fflush(log);
    reboot(0x408);  /* Same power-down mode as the iPad restore tool. */
    return 0;
}
#endif
