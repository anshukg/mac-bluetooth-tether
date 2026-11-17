#include "BroadcomAddressTest.h"

#include <stddef.h>

static void SetAddressCommand(bd_addr_t address, uint8_t *command) {
    if (address == NULL || command == NULL) return;
    command[0] = 0x01;
    command[1] = 0xfc;
    command[2] = 0x06;
    for (size_t index = 0; index < 6; ++index) {
        command[3 + index] = address[5 - index];
    }
}

const btstack_chipset_t *BroadcomAddressTestChipset(void) {
    static const btstack_chipset_t chipset = {
        .name = "Broadcom temporary address test",
        .init = NULL,
        .next_command = NULL,
        .set_baudrate_command = NULL,
        .set_bd_addr_command = SetAddressCommand,
    };
    return &chipset;
}
