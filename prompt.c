#define _GNU_SOURCE
#include "prompt.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "config.h"
#include "disk.h"
#include "util.h"

typedef enum { ASK_OK, ASK_CANCEL, ASK_GONE, ASK_UNAVAILABLE, ASK_ABORT } AskResult;

// zenity password window. Polls while the dialog is open so it can be closed if the
// drive is unplugged or the program is stopped.
static AskResult ask_password_gui(const char* disk, int attempt, char* out, size_t out_len) {
    char text[300];
    if (attempt == 1)
        snprintf(text, sizeof(text),
                 "--text=USB drive detected: /dev/%s\n\nEnter the password to unlock "
                 "(attempt %d of %d):",
                 disk, attempt, MAX_ATTEMPTS);
    else
        snprintf(text, sizeof(text),
                 "--text=Incorrect password!\n\nUnlock /dev/%s (attempt %d of %d):", disk, attempt,
                 MAX_ATTEMPTS);

    char* argv[] = {"zenity", "--password", "--title=USB Security Guard", text, NULL};
    int fd = -1;
    pid_t pid = spawn_as_user(argv, &fd);
    if (pid < 0)
        return ASK_UNAVAILABLE;

    size_t len = 0;
    out[0] = '\0';
    bool gone = false, abort_req = false;

    for (;;) {
        if (!g_running) {
            abort_req = true;
            break;
        }
        if (!blockExists(disk)) {
            gone = true;
            break;
        }
        struct pollfd pfd = {fd, POLLIN, 0};
        int pr = poll(&pfd, 1, 200);
        if (pr > 0) {
            char tmp[256];
            ssize_t n = read(fd, tmp, sizeof(tmp));
            if (n > 0) {
                for (ssize_t i = 0; i < n && len < out_len - 1; i++)
                    out[len++] = tmp[i];
                out[len] = '\0';
            } else if (n == 0 || errno != EINTR) {
                break; // dialog closed
            }
        }
    }

    if (gone || abort_req)
        kill(pid, SIGTERM);
    close(fd);

    int status = 0;
    pid_t r;
    do {
        r = waitpid(pid, &status, 0);
    } while (r < 0 && errno == EINTR);

    if (abort_req)
        return ASK_ABORT;
    if (gone)
        return ASK_GONE;

    out[strcspn(out, "\r\n")] = '\0';
    if (r > 0 && WIFEXITED(status)) {
        if (WEXITSTATUS(status) == 0)
            return ASK_OK;
        if (WEXITSTATUS(status) == 1)
            return ASK_CANCEL; // user pressed Cancel / closed the window
    }
    return ASK_UNAVAILABLE; // zenity missing or could not reach the display
}

static void get_secure_password(char* buf, size_t length) {
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    if (fgets(buf, length, stdin) != NULL) {
        buf[strcspn(buf, "\r\n")] = '\0';
    } else {
        buf[0] = '\0';
        clearerr(stdin);
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
}

// Terminal fallback, used only when the GUI cannot be shown
static AskResult ask_password_terminal(const char* disk, int attempt, char* out, size_t len) {
    if (!isatty(STDIN_FILENO))
        return ASK_UNAVAILABLE;
    printf("\n[SECURITY] USB drive /dev/%s detected. Enter password (attempt %d/%d): ", disk,
           attempt, MAX_ATTEMPTS);
    fflush(stdout);
    tcflush(STDIN_FILENO, TCIFLUSH);
    get_secure_password(out, len);
    printf("\n");
    if (!g_running)
        return ASK_ABORT;
    return out[0] ? ASK_OK : ASK_CANCEL;
}

VerifyResult verifyPassword(const char* disk) {
    char password[128];
    bool use_gui = true;
    int attempt = 1;

    while (attempt <= MAX_ATTEMPTS) {
        if (!g_running)
            return V_ABORT;
        if (!blockExists(disk))
            return V_GONE;

        AskResult r;
        if (use_gui) {
            r = ask_password_gui(disk, attempt, password, sizeof(password));
            if (r == ASK_UNAVAILABLE) {
                // Not a wrong password: switch to terminal, attempt is NOT used up
                printf("[!] Password window could not be shown, using the terminal.\n");
                use_gui = false;
                continue;
            }
        } else {
            r = ask_password_terminal(disk, attempt, password, sizeof(password));
            if (r == ASK_UNAVAILABLE) {
                printf("[!] No GUI and no terminal available. Drive stays locked.\n");
                return V_ABORT;
            }
        }

        if (r == ASK_GONE)
            return V_GONE;
        if (r == ASK_ABORT)
            return V_ABORT;
        if (r == ASK_OK && strcmp(password, CORRECT_PASSWORD) == 0)
            return V_OK;

        printf("[-] Incorrect password or cancelled (attempt %d of %d).\n", attempt, MAX_ATTEMPTS);
        attempt++;
    }
    return V_DENIED;
}
