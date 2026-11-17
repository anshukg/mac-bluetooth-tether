#ifndef SKYWALK_HCI_DRIVER_H
#define SKYWALK_HCI_DRIVER_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    SKYWALK_HCI_PIPE_HCI = 0,
    SKYWALK_HCI_PIPE_ACL = 1,
    SKYWALK_HCI_PIPE_SCO = 2,
    SKYWALK_HCI_PIPE_ISO = 3,
    SKYWALK_HCI_PIPE_COUNT = 4,
} SkywalkHCIPipe;

enum {
    SKYWALK_HCI_SEND_COMPLETE = 0,
    SKYWALK_HCI_SEND_PENDING = 1,
};

/* One driver owns all four pipes.  receive_packet returns one complete HCI
 * packet, zero when the pipe has no packet, or a negative errno value. */
typedef struct {
    void *context;
    int (*open)(void *context);
    void (*close)(void *context);
    int (*file_descriptor)(void *context, SkywalkHCIPipe pipe);
    int (*can_send_packet_now)(void *context);
    int (*send_packet)(void *context,
                       SkywalkHCIPipe pipe,
                       const uint8_t *packet,
                       size_t size);
    /* Returns 1 when the last accepted send has finished, 0 while it is
     * still pending, or a negative errno value. */
    int (*poll_transmit)(void *context);
    int (*receive_packet)(void *context,
                          SkywalkHCIPipe pipe,
                          uint8_t *packet,
                          size_t capacity,
                          size_t *size);
} SkywalkHCIDriver;

typedef void (*SkywalkHCIOpenWaitCallback)(void *context);

const SkywalkHCIDriver *SkywalkHCINativeDriver(void);
void SkywalkHCINativeDriverSetOpenTimeoutMilliseconds(uint32_t timeout_ms);
void SkywalkHCINativeDriverSetOpenWaitCallback(
    SkywalkHCIOpenWaitCallback callback,
    void *context);
void SkywalkHCINativeDriverCancelOpen(void);

/* Returns the HCI packet size carried in a Skywalk slot.  Apple may add an
 * eight-byte PCIe footer after the packet.  A negative errno value means the
 * slot is malformed. */
int SkywalkHCIPacketLength(SkywalkHCIPipe pipe,
                          const uint8_t *slot,
                          size_t slot_size);

#endif
