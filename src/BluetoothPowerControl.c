#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

extern int IOBluetoothPreferenceGetControllerPowerState(void);
extern void IOBluetoothPreferenceSetControllerPowerState(int state);

enum {
    kDefaultWaitMilliseconds = 10000,
    kPollMilliseconds = 25,
};

static void print_usage(FILE *stream, const char *program) {
    fprintf(stream,
            "usage: %s get\n"
            "       %s set <0|1|off|on> [--wait-ms milliseconds]\n",
            program,
            program);
}

static int parse_power_state(const char *value, int *state) {
    if (strcmp(value, "0") == 0 || strcmp(value, "off") == 0) {
        *state = 0;
        return 0;
    }
    if (strcmp(value, "1") == 0 || strcmp(value, "on") == 0) {
        *state = 1;
        return 0;
    }
    return -1;
}

static int parse_milliseconds(const char *value, int *milliseconds) {
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed < 0 ||
        parsed > INT_MAX) {
        return -1;
    }
    *milliseconds = (int)parsed;
    return 0;
}

static void sleep_milliseconds(int milliseconds) {
    struct timespec delay;

    delay.tv_sec = milliseconds / 1000;
    delay.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    while (nanosleep(&delay, &delay) == -1 && errno == EINTR) {
    }
}

static int wait_for_power_state(int wanted, int wait_milliseconds) {
    int elapsed = 0;

    for (;;) {
        int actual = IOBluetoothPreferenceGetControllerPowerState();
        if (actual == wanted) {
            return 0;
        }
        if (elapsed >= wait_milliseconds) {
            fprintf(stderr,
                    "Bluetooth power did not become %d within %d ms; current state is %d\n",
                    wanted,
                    wait_milliseconds,
                    actual);
            return 4;
        }
        int sleep_for = kPollMilliseconds;
        if (sleep_for > wait_milliseconds - elapsed) {
            sleep_for = wait_milliseconds - elapsed;
        }
        sleep_milliseconds(sleep_for);
        elapsed += sleep_for;
    }
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "get") == 0) {
        int state = IOBluetoothPreferenceGetControllerPowerState();
        if (state != 0 && state != 1) {
            fprintf(stderr, "Bluetooth power returned an unknown state: %d\n", state);
            return 3;
        }
        printf("%d\n", state);
        return 0;
    }

    if (argc >= 3 && strcmp(argv[1], "set") == 0) {
        int state = 0;
        int wait_milliseconds = kDefaultWaitMilliseconds;
        if (parse_power_state(argv[2], &state) != 0) {
            print_usage(stderr, argv[0]);
            return 2;
        }
        if (argc == 5 && strcmp(argv[3], "--wait-ms") == 0) {
            if (parse_milliseconds(argv[4], &wait_milliseconds) != 0) {
                print_usage(stderr, argv[0]);
                return 2;
            }
        } else if (argc != 3) {
            print_usage(stderr, argv[0]);
            return 2;
        }

        IOBluetoothPreferenceSetControllerPowerState(state);
        return wait_for_power_state(state, wait_milliseconds);
    }

    if (argc == 2 &&
        (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0)) {
        print_usage(stdout, argv[0]);
        return 0;
    }

    print_usage(stderr, argv[0]);
    return 2;
}
