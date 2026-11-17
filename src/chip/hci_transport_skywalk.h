/*
 * This transport uses the BTstack hci_transport_t interface.  BTstack is
 * vendored with its original BlueKitchen license in the .vendor directory.
 */

#ifndef HCI_TRANSPORT_SKYWALK_H
#define HCI_TRANSPORT_SKYWALK_H

#include "SkywalkHCIDriver.h"
#include "hci_transport.h"

/* The returned object is a BTstack singleton, matching BTstack's other HCI
 * transports.  The driver must stay alive until close returns. */
const hci_transport_t *hci_transport_skywalk_instance(
    const SkywalkHCIDriver *driver);

typedef void (*SkywalkHCITransportFaultHandler)(void *context, int error);
void hci_transport_skywalk_set_fault_handler(
    SkywalkHCITransportFaultHandler handler,
    void *context);

#ifdef HCI_TRANSPORT_SKYWALK_TESTING
void hci_transport_skywalk_process_pipe_for_testing(SkywalkHCIPipe pipe);
void hci_transport_skywalk_process_transmit_for_testing(void);
void hci_transport_skywalk_set_now_for_testing(uint64_t now_ms);
void hci_transport_skywalk_act_event_for_testing(uint32_t type,
                                                  const char *message,
                                                  uint32_t value);
#endif

#endif
