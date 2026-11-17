#ifndef ANDROID_MAC_CALL_BRIDGE_BROADCOM_ADDRESS_TEST_H
#define ANDROID_MAC_CALL_BRIDGE_BROADCOM_ADDRESS_TEST_H

#include "btstack_chipset.h"

/* Test-only chipset hook for a temporary Broadcom controller address. */
const btstack_chipset_t *BroadcomAddressTestChipset(void);

#endif
