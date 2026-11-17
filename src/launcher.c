// launcher.c — main executable of "Bluetooth Tether.app". macOS starts it at login.
// It runs the automatic-mode loop (`bt-tether _auto`) as a child and stays alive as its parent,
// so that macOS attributes Bluetooth access to this app and asks for permission once.
//
// Layout it expects:
//   <base>/Bluetooth Tether.app/Contents/MacOS/BluetoothTether   (this program)
//   <base>/app/bt-tether                                          (the script it runs)
//
// Build: clang launcher.c -o build/BluetoothTether

#include <limits.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
static pid_t child_pid;

static void Forward(int signal_number) {
    if (child_pid > 0) kill(child_pid, signal_number);
}

int main(void) {
    char executable[PATH_MAX];
    uint32_t size = sizeof executable;
    if (_NSGetExecutablePath(executable, &size) != 0) return 2;
    char resolved[PATH_MAX];
    if (realpath(executable, resolved) == NULL) return 2;
    char *bundle = strstr(resolved, "/Bluetooth Tether.app/");
    if (bundle == NULL) return 2;
    *bundle = '\0';

    char script[PATH_MAX];
    snprintf(script, sizeof script, "%s/app/bt-tether", resolved);
    if (access(script, R_OK) != 0) {
        fprintf(stderr, "Bluetooth Tether: %s is missing\n", script);
        return 3;
    }

    signal(SIGTERM, Forward);
    signal(SIGINT, Forward);
    char *arguments[] = {"/bin/zsh", script, "_auto", NULL};
    if (posix_spawn(&child_pid, "/bin/zsh", NULL, NULL, arguments, environ) != 0) return 4;

    int status = 0;
    while (waitpid(child_pid, &status, 0) < 0) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}
