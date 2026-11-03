#define _GNU_SOURCE
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

volatile sig_atomic_t g_running = 1;

static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

void install_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal; // no SA_RESTART, so blocking calls are interrupted
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
}

// Sleep in small slices so Ctrl+C is noticed quickly
void sleep_ms(int ms) {
    while (ms > 0 && g_running) {
        int slice = ms > 100 ? 100 : ms;
        usleep(slice * 1000);
        ms -= slice;
    }
}

void mkdir_p(const char* path) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static int wait_for(pid_t pid) {
    int status = 0;
    pid_t r;
    do {
        r = waitpid(pid, &status, 0);
    } while (r < 0 && errno == EINTR);
    if (r < 0 || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

// Run a command as root, silently, wait for it. Returns its exit code (-1 on failure).
int run_cmd(char* const argv[]) {
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) {
            dup2(nul, 0);
            dup2(nul, 1);
            dup2(nul, 2);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    return wait_for(pid);
}

// In a child process: become the user who ran sudo and rebuild the desktop environment
// (sudo strips it). Works for both Wayland (Ubuntu 24.04 default) and X11.
static bool drop_to_user(void) {
    const char* uid_s = getenv("SUDO_UID");
    const char* gid_s = getenv("SUDO_GID");
    if (!uid_s || !gid_s)
        return true; // started as a real root login, keep running as root

    uid_t uid = (uid_t)strtoul(uid_s, NULL, 10);
    gid_t gid = (gid_t)strtoul(gid_s, NULL, 10);
    struct passwd* pw = getpwuid(uid);

    if (pw)
        initgroups(pw->pw_name, gid);
    if (setgid(gid) != 0 || setuid(uid) != 0)
        return false;

    char buf[128];
    snprintf(buf, sizeof(buf), "/run/user/%u", (unsigned)uid);
    setenv("XDG_RUNTIME_DIR", buf, 1);
    snprintf(buf, sizeof(buf), "unix:path=/run/user/%u/bus", (unsigned)uid);
    setenv("DBUS_SESSION_BUS_ADDRESS", buf, 1);

    if (!getenv("DISPLAY"))
        setenv("DISPLAY", ":0", 1);
    if (!getenv("WAYLAND_DISPLAY"))
        setenv("WAYLAND_DISPLAY", "wayland-0", 1);

    snprintf(buf, sizeof(buf), "/run/user/%u/gdm/Xauthority", (unsigned)uid);
    if (!getenv("XAUTHORITY") && access(buf, R_OK) == 0)
        setenv("XAUTHORITY", buf, 1);

    if (pw) {
        setenv("HOME", pw->pw_dir, 1);
        setenv("USER", pw->pw_name, 1);
    }
    return true;
}

// Start a program as the desktop user without waiting for it.
// If out_fd is not NULL, the program's stdout can be read from *out_fd.
pid_t spawn_as_user(char* const argv[], int* out_fd) {
    int fds[2] = {-1, -1};
    if (out_fd && pipe(fds) < 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        if (out_fd) {
            close(fds[0]);
            close(fds[1]);
        }
        return -1;
    }
    if (pid == 0) {
        setsid(); // own session: terminal Ctrl+C does not hit it, we kill it ourselves
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) {
            dup2(nul, 0);
            dup2(nul, 2);
            if (!out_fd)
                dup2(nul, 1);
        }
        if (out_fd) {
            dup2(fds[1], 1);
            close(fds[0]);
            close(fds[1]);
        }
        if (!drop_to_user())
            _exit(126);
        execvp(argv[0], argv);
        _exit(127);
    }
    if (out_fd) {
        close(fds[1]);
        *out_fd = fds[0];
    }
    return pid;
}

int run_as_user(char* const argv[]) {
    pid_t pid = spawn_as_user(argv, NULL);
    if (pid < 0)
        return -1;
    return wait_for(pid);
}

// Fire and forget (double fork so no zombie is left behind)
void spawn_detached(char* const argv[]) {
    pid_t p = fork();
    if (p == 0) {
        spawn_as_user(argv, NULL);
        _exit(0);
    }
    if (p > 0)
        waitpid(p, NULL, 0);
}

void show_gui_message(const char* type, const char* title, const char* message) {
    char flag[32], t[128], m[256];
    snprintf(flag, sizeof(flag), "--%s", type);
    snprintf(t, sizeof(t), "--title=%s", title);
    snprintf(m, sizeof(m), "--text=%s", message);
    char* argv[] = {"zenity", flag, t, m, "--timeout=5", NULL};
    spawn_detached(argv);
}
