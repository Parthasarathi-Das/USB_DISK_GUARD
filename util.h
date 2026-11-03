#ifndef UTIL_H
#define UTIL_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

extern volatile sig_atomic_t g_running;

void install_signal_handlers(void);
void sleep_ms(int ms);
void mkdir_p(const char* path);

int run_cmd(char* const argv[]);                          // as root, silent, waits
int run_as_user(char* const argv[]);                      // as the desktop user, silent, waits
pid_t spawn_as_user(char* const argv[], int* out_fd);     // as the desktop user, no wait
void spawn_detached(char* const argv[]);                  // fire and forget
void show_gui_message(const char* type, const char* title, const char* message);

#endif
