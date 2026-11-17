#include "SkywalkHCIDriver.h"

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    HCI_OPCODE_RESET = 0x0c03,
    HCI_OPCODE_READ_BD_ADDR = 0x1009,
    HCI_OPCODE_BROADCOM_WRITE_BD_ADDR = 0xfc01,
    HCI_EVENT_COMMAND_COMPLETE = 0x0e,
    HCI_COMMAND_COMPLETE_HEADER_SIZE = 6,
};

static volatile sig_atomic_t stop_requested;
static volatile sig_atomic_t restore_in_progress;

static void Stop(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
    SkywalkHCINativeDriverCancelOpen();
}

static uint16_t ReadLittleEndian16(const uint8_t *bytes) {
    return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

static int HexValue(char character) {
    if (character >= '0' && character <= '9') return character - '0';
    character = (char)tolower((unsigned char)character);
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    return -1;
}

/* Store addresses in the human-readable order used by AA:BB:CC:DD:EE:FF. */
static bool ParseAddress(const char *text, uint8_t address[6]) {
    if (text == NULL || strlen(text) != 17) return false;
    for (size_t index = 0; index < 6; ++index) {
        size_t offset = index * 3;
        int high = HexValue(text[offset]);
        int low = HexValue(text[offset + 1]);
        if (high < 0 || low < 0) return false;
        if (index < 5 && text[offset + 2] != ':') return false;
        address[index] = (uint8_t)((high << 4) | low);
    }
    return true;
}

static void FormatAddress(const uint8_t address[6], char text[18]) {
    (void)snprintf(text,
                   18,
                   "%02X:%02X:%02X:%02X:%02X:%02X",
                   address[0],
                   address[1],
                   address[2],
                   address[3],
                   address[4],
                   address[5]);
}

static bool AddressesEqual(const uint8_t left[6], const uint8_t right[6]) {
    return memcmp(left, right, 6) == 0;
}

static int WaitForCommandComplete(const SkywalkHCIDriver *driver,
                                  uint16_t opcode,
                                  uint8_t *event,
                                  size_t capacity,
                                  size_t *event_size) {
    struct pollfd descriptor = {
        .fd = driver->file_descriptor(driver->context,
                                      SKYWALK_HCI_PIPE_HCI),
        .events = POLLIN,
    };
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (stop_requested && !restore_in_progress) return -ECANCELED;
        int poll_result = poll(&descriptor, 1, 100);
        if (poll_result < 0) {
            if (errno == EINTR) continue;
            return -errno;
        }
        if (poll_result == 0) continue;
        for (;;) {
            size_t size = 0;
            int receive_result = driver->receive_packet(
                driver->context,
                SKYWALK_HCI_PIPE_HCI,
                event,
                capacity,
                &size);
            if (receive_result < 0) return receive_result;
            if (receive_result == 0) break;
            if (size >= HCI_COMMAND_COMPLETE_HEADER_SIZE &&
                event[0] == HCI_EVENT_COMMAND_COMPLETE &&
                ReadLittleEndian16(&event[3]) == opcode) {
                *event_size = size;
                return event[5] == 0 ? 0 : -(int)event[5];
            }
        }
    }
    return -ETIMEDOUT;
}

static int SendCommand(const SkywalkHCIDriver *driver,
                       const uint8_t *command,
                       size_t command_size,
                       uint8_t *event,
                       size_t event_capacity,
                       size_t *event_size) {
    uint16_t opcode = ReadLittleEndian16(command);
    int result = driver->send_packet(driver->context,
                                     SKYWALK_HCI_PIPE_HCI,
                                     command,
                                     command_size);
    if (result != 0) return result;
    return WaitForCommandComplete(driver,
                                  opcode,
                                  event,
                                  event_capacity,
                                  event_size);
}

static int ResetController(const SkywalkHCIDriver *driver) {
    const uint8_t command[] = {0x03, 0x0c, 0x00};
    uint8_t event[64];
    size_t event_size = 0;
    return SendCommand(driver,
                       command,
                       sizeof(command),
                       event,
                       sizeof(event),
                       &event_size);
}

static int ReadControllerAddress(const SkywalkHCIDriver *driver,
                                 uint8_t address[6]) {
    const uint8_t command[] = {0x09, 0x10, 0x00};
    uint8_t event[64];
    size_t event_size = 0;
    int result = SendCommand(driver,
                             command,
                             sizeof(command),
                             event,
                             sizeof(event),
                             &event_size);
    if (result != 0) return result;
    if (event_size < 12) return -EPROTO;

    /* HCI carries a Bluetooth address least-significant byte first. */
    for (size_t index = 0; index < 6; ++index) {
        address[index] = event[11 - index];
    }
    return 0;
}

static int WriteControllerAddress(const SkywalkHCIDriver *driver,
                                  const uint8_t address[6]) {
    uint8_t command[9] = {0x01, 0xfc, 0x06};
    for (size_t index = 0; index < 6; ++index) {
        command[3 + index] = address[5 - index];
    }
    uint8_t event[64];
    size_t event_size = 0;
    return SendCommand(driver,
                       command,
                       sizeof(command),
                       event,
                       sizeof(event),
                       &event_size);
}

static int ReadAndPrintAddress(const SkywalkHCIDriver *driver) {
    uint8_t address[6];
    int result = ReadControllerAddress(driver, address);
    if (result != 0) return result;
    char text[18];
    FormatAddress(address, text);
    printf("controllerAddress=%s\n", text);
    return 0;
}

static int RestoreAndVerify(const SkywalkHCIDriver *driver,
                            const uint8_t original[6]) {
    int result = WriteControllerAddress(driver, original);
    if (result != 0) return result;
    uint8_t actual[6];
    result = ReadControllerAddress(driver, actual);
    if (result != 0) return result;
    return AddressesEqual(actual, original) ? 0 : -EADDRNOTAVAIL;
}

static void PrintUsage(FILE *stream, const char *program) {
    fprintf(stream,
            "usage: %s --read-address\n"
            "       %s --temporary-address-smoke "
            "EXPECTED_ORIGINAL TEMPORARY\n"
            "       %s --restore-address EXPECTED_CURRENT ORIGINAL\n",
            program,
            program,
            program);
}

int main(int argc, const char *argv[]) {
    bool read_only = argc == 2 && strcmp(argv[1], "--read-address") == 0;
    bool smoke = argc == 4 &&
        strcmp(argv[1], "--temporary-address-smoke") == 0;
    bool restore_only = argc == 4 &&
        strcmp(argv[1], "--restore-address") == 0;
    if (!read_only && !smoke && !restore_only) {
        PrintUsage(stderr, argv[0]);
        return 2;
    }

    uint8_t expected_original[6] = {0};
    uint8_t temporary[6] = {0};
    if ((smoke || restore_only) &&
        (!ParseAddress(argv[2], expected_original) ||
         !ParseAddress(argv[3], temporary) ||
         AddressesEqual(expected_original, temporary))) {
        PrintUsage(stderr, argv[0]);
        return 2;
    }

    signal(SIGINT, Stop);
    signal(SIGTERM, Stop);
    signal(SIGHUP, Stop);

    const SkywalkHCIDriver *driver = SkywalkHCINativeDriver();
    SkywalkHCINativeDriverSetOpenTimeoutMilliseconds(12000);
    int result = driver->open(driver->context);
    if (result != 0) {
        fprintf(stderr, "Could not open the Bluetooth controller: %d\n", result);
        return 3;
    }

    result = ResetController(driver);
    if (result != 0) {
        fprintf(stderr, "HCI Reset failed: %d\n", result);
        driver->close(driver->context);
        return 4;
    }

    if (read_only) {
        result = ReadAndPrintAddress(driver);
        if (result != 0) {
            fprintf(stderr, "Read BD_ADDR failed: %d\n", result);
        }
        driver->close(driver->context);
        return result == 0 ? 0 : 5;
    }

    uint8_t actual[6];
    result = ReadControllerAddress(driver, actual);
    if (restore_only && result == 0 && AddressesEqual(actual, temporary)) {
        char text[18];
        FormatAddress(actual, text);
        printf("restoredAddress=%s verified=true alreadyRestored=true\n", text);
        driver->close(driver->context);
        return 0;
    }
    if (result != 0 || !AddressesEqual(actual, expected_original)) {
        char text[18] = "unreadable";
        if (result == 0) FormatAddress(actual, text);
        fprintf(stderr,
                "Refusing the write because the controller address is %s\n",
                text);
        driver->close(driver->context);
        return 6;
    }

    if (restore_only) {
        sigset_t restore_signals;
        sigset_t old_signals;
        sigemptyset(&restore_signals);
        sigaddset(&restore_signals, SIGINT);
        sigaddset(&restore_signals, SIGTERM);
        sigaddset(&restore_signals, SIGHUP);
        (void)sigprocmask(SIG_BLOCK, &restore_signals, &old_signals);
        restore_in_progress = 1;
        int restore_result = RestoreAndVerify(driver, temporary);
        restore_in_progress = 0;
        (void)sigprocmask(SIG_SETMASK, &old_signals, NULL);
        if (restore_result == 0) {
            char text[18];
            FormatAddress(temporary, text);
            printf("restoredAddress=%s verified=true alreadyRestored=false\n",
                   text);
        } else {
            fprintf(stderr,
                    "Factory address restore could not be proved: %d\n",
                    restore_result);
        }
        driver->close(driver->context);
        return restore_result == 0 ? 0 : 8;
    }

    bool temporary_write_attempted = true;
    result = WriteControllerAddress(driver, temporary);
    if (result == 0) result = ReadControllerAddress(driver, actual);
    if (result == 0 && !AddressesEqual(actual, temporary)) {
        result = -EADDRNOTAVAIL;
    }
    if (result != 0) {
        fprintf(stderr, "Temporary address verification failed: %d\n", result);
    } else {
        char text[18];
        FormatAddress(actual, text);
        printf("temporaryAddress=%s verified=true\n", text);
    }

    sigset_t restore_signals;
    sigset_t old_signals;
    sigemptyset(&restore_signals);
    sigaddset(&restore_signals, SIGINT);
    sigaddset(&restore_signals, SIGTERM);
    sigaddset(&restore_signals, SIGHUP);
    (void)sigprocmask(SIG_BLOCK, &restore_signals, &old_signals);
    restore_in_progress = 1;
    int restore_result = temporary_write_attempted
        ? RestoreAndVerify(driver, expected_original)
        : 0;
    restore_in_progress = 0;
    (void)sigprocmask(SIG_SETMASK, &old_signals, NULL);
    if (restore_result != 0) {
        fprintf(stderr,
                "Factory address restore could not be proved: %d\n",
                restore_result);
    } else {
        char text[18];
        FormatAddress(expected_original, text);
        printf("restoredAddress=%s verified=true\n", text);
    }
    driver->close(driver->context);

    if (restore_result != 0) return 8;
    return result == 0 ? 0 : 7;
}
