#include "SkywalkHCIDriver.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sched.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <time.h>
#include <unistd.h>
#include <uuid/uuid.h>

/* Skywalk's channel API ships in libSystem but its header is private.  These
 * declarations match Apple's open-source os_channel.h ABI. */
struct channel;
struct channel_ring_desc;
struct __slot_desc;
typedef struct channel *channel_t;
typedef struct channel_ring_desc *channel_ring_t;
typedef struct __slot_desc *channel_slot_t;
typedef uint16_t nexus_port_t;
typedef uint32_t ring_id_t;

typedef struct slot_prop {
    uint16_t flags;
    uint16_t length;
    uint32_t index;
    uint64_t external_buffer;
    uint64_t buffer;
    uint64_t metadata;
    uint32_t reserved[8];
} slot_prop_t __attribute__((aligned(sizeof(uint64_t))));

enum {
    CHANNEL_FIRST_TX_RING = 0,
    CHANNEL_FIRST_RX_RING = 2,
    CHANNEL_SYNC_TX = 0,
    CHANNEL_SYNC_RX = 1,
};

extern channel_t os_channel_create(const uuid_t uuid, nexus_port_t port);
extern void os_channel_destroy(channel_t channel);
extern int os_channel_get_fd(channel_t channel);
extern int os_channel_sync(channel_t channel, int mode);
extern ring_id_t os_channel_ring_id(channel_t channel, int type);
extern channel_ring_t os_channel_tx_ring(channel_t channel, ring_id_t ring_id);
extern channel_ring_t os_channel_rx_ring(channel_t channel, ring_id_t ring_id);
extern uint32_t os_channel_available_slot_count(channel_ring_t ring);
extern int os_channel_pending(channel_ring_t ring);
extern int os_channel_is_defunct(channel_t channel);
extern uint64_t os_channel_ring_sync_time(channel_ring_t ring);
extern uint64_t os_channel_ring_notify_time(channel_ring_t ring);
extern channel_slot_t os_channel_get_next_slot(channel_ring_t ring,
                                               channel_slot_t previous,
                                               slot_prop_t *properties);
extern int os_channel_advance_slot(channel_ring_t ring,
                                   channel_slot_t slot);
extern void os_channel_set_slot_properties(channel_ring_t ring,
                                           channel_slot_t slot,
                                           const slot_prop_t *properties);

typedef struct {
    channel_t channel;
    channel_ring_t transmit_ring;
    channel_ring_t receive_ring;
    int file_descriptor;
    int transmit_kqueue;
    int receive_kqueue;
} NativePipe;

typedef struct {
    NativePipe pipes[SKYWALK_HCI_PIPE_COUNT];
    uuid_t pipe_uuids[SKYWALK_HCI_PIPE_COUNT];
    _Atomic bool cancel_open;
    _Atomic uint32_t open_timeout_ms;
    SkywalkHCIOpenWaitCallback open_wait_callback;
    void *open_wait_context;
    bool open_wait_reported;
} NativeDriverContext;

static NativeDriverContext native_context;
static unsigned int native_debug_send_count;
static unsigned int native_debug_late_receive_count;

static int NativeStatus(int result) {
    if (result == 0) return 0;
    if (result == -1) return -(errno != 0 ? errno : EIO);
    return result < 0 ? result : -result;
}

static int NativeWaitForKqueueEvent(int kqueue_descriptor,
                                    int16_t expected_filter,
                                    int timeout_ms) {
    if (kqueue_descriptor < 0) return -EBADF;
    struct kevent event;
    struct timespec timeout = {
        .tv_sec = timeout_ms / 1000,
        .tv_nsec = (long)(timeout_ms % 1000) * 1000000L,
    };
    for (;;) {
        errno = 0;
        int result = kevent(kqueue_descriptor,
                            NULL,
                            0,
                            &event,
                            1,
                            &timeout);
        if (result > 0) {
            if ((event.flags & EV_ERROR) != 0 && event.data != 0) {
                return -(int)event.data;
            }
            return event.filter == expected_filter ? 1 : -EPROTO;
        }
        if (result == 0) return 0;
        if (errno != EINTR) return -errno;
    }
}

static int NativeRegisterKqueue(int file_descriptor, int16_t filter) {
    int descriptor = kqueue();
    if (descriptor < 0) return -errno;
    struct kevent change;
    EV_SET(&change,
           (uintptr_t)file_descriptor,
           filter,
           EV_ADD | EV_ENABLE,
           0,
           0,
           NULL);
    errno = 0;
    if (kevent(descriptor, &change, 1, NULL, 0, NULL) != 0) {
        int error = errno != 0 ? errno : EIO;
        close(descriptor);
        return -error;
    }
    return descriptor;
}

static int NativeSyncChannel(NativePipe *pipe, int mode) {
    if (pipe == NULL || pipe->channel == NULL) return -ENODEV;
    errno = 0;
    return NativeStatus(os_channel_sync(pipe->channel, mode));
}

static int CopySkywalkUUIDForProtocol(const char *protocol, uuid_t output) {
    if (protocol == NULL || output == NULL) return EINVAL;

    CFMutableDictionaryRef matching =
        IOServiceMatching("AppleConvergedIPCInterface");
    if (matching == NULL) return ENOMEM;
    CFStringRef protocol_value = CFStringCreateWithCString(
        kCFAllocatorDefault, protocol, kCFStringEncodingUTF8);
    if (protocol_value == NULL) {
        CFRelease(matching);
        return ENOMEM;
    }
    CFDictionarySetValue(matching,
                         CFSTR("ACIPCInterfaceProtocol"),
                         protocol_value);
    CFRelease(protocol_value);

    io_iterator_t iterator = IO_OBJECT_NULL;
    kern_return_t io_result = IOServiceGetMatchingServices(
        kIOMainPortDefault, matching, &iterator);
    if (io_result != KERN_SUCCESS) return EIO;

    int result = ENOENT;
    io_service_t service;
    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        CFTypeRef transport_value = IORegistryEntryCreateCFProperty(
            service,
            CFSTR("ACIPCInterfaceTransport"),
            kCFAllocatorDefault,
            0);
        bool is_skywalk = transport_value != NULL &&
            CFGetTypeID(transport_value) == CFStringGetTypeID() &&
            CFStringCompare((CFStringRef)transport_value,
                            CFSTR("skywalk"),
                            0) == kCFCompareEqualTo;
        if (transport_value != NULL) CFRelease(transport_value);
        if (!is_skywalk) {
            IOObjectRelease(service);
            continue;
        }

        CFTypeRef uuid_value = IORegistryEntrySearchCFProperty(
            service,
            kIOServicePlane,
            CFSTR("IOSkywalkNexusUUID"),
            kCFAllocatorDefault,
            kIORegistryIterateRecursively);
        if (uuid_value != NULL &&
            CFGetTypeID(uuid_value) == CFStringGetTypeID()) {
            char uuid_text[37] = {0};
            if (CFStringGetCString((CFStringRef)uuid_value,
                                   uuid_text,
                                   sizeof(uuid_text),
                                   kCFStringEncodingUTF8) &&
                uuid_parse(uuid_text, output) == 0) {
                result = 0;
            } else {
                result = EINVAL;
            }
        }
        if (uuid_value != NULL) CFRelease(uuid_value);
        IOObjectRelease(service);
        if (result == 0) break;
    }
    IOObjectRelease(iterator);
    return result;
}

static void NativeReleasePipe(NativePipe *pipe) {
    if (pipe == NULL) return;
    if (pipe->receive_kqueue >= 0) close(pipe->receive_kqueue);
    if (pipe->transmit_kqueue >= 0) close(pipe->transmit_kqueue);
    if (pipe->channel != NULL) os_channel_destroy(pipe->channel);
    memset(pipe, 0, sizeof(*pipe));
    pipe->file_descriptor = -1;
    pipe->transmit_kqueue = -1;
    pipe->receive_kqueue = -1;
}

static void NativeReleasePipes(NativeDriverContext *context) {
    if (context == NULL) return;
    for (int index = SKYWALK_HCI_PIPE_COUNT - 1; index >= 0; --index) {
        NativeReleasePipe(&context->pipes[index]);
    }
}

static void NativeClose(void *opaque_context) {
    NativeDriverContext *context = opaque_context;
    if (context == NULL) return;
    atomic_store_explicit(&context->cancel_open, true, memory_order_release);
    NativeReleasePipes(context);
}

static uint64_t MonotonicMilliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static bool OpenDeadlinePassed(uint64_t deadline_ms) {
    return deadline_ms != 0 && MonotonicMilliseconds() >= deadline_ms;
}

static void PauseForPipe(SkywalkHCIPipe pipe) {
    if (pipe == SKYWALK_HCI_PIPE_HCI) {
        sched_yield();
        return;
    }
    const struct timespec pause = {.tv_sec = 0, .tv_nsec = 100000};
    nanosleep(&pause, NULL);
}

static int NativeOpenPipe(NativeDriverContext *context,
                          SkywalkHCIPipe pipe_kind,
                          uint64_t deadline_ms) {
    if (context == NULL || pipe_kind < 0 ||
        pipe_kind >= SKYWALK_HCI_PIPE_COUNT) return -EINVAL;
    NativePipe *pipe = &context->pipes[pipe_kind];
    NativeReleasePipe(pipe);
    for (;;) {
        if (atomic_load_explicit(&context->cancel_open,
                                 memory_order_acquire)) return -ECANCELED;
        if (OpenDeadlinePassed(deadline_ms)) return -ETIMEDOUT;
        errno = 0;
        pipe->channel = os_channel_create(context->pipe_uuids[pipe_kind], 0);
        if (pipe->channel != NULL) break;
        int error = errno != 0 ? errno : EIO;
        if (error != EBUSY && error != EAGAIN) return -error;
        if (pipe_kind == SKYWALK_HCI_PIPE_HCI &&
            !context->open_wait_reported) {
            context->open_wait_reported = true;
            if (context->open_wait_callback != NULL) {
                context->open_wait_callback(context->open_wait_context);
            }
        }
        PauseForPipe(pipe_kind);
    }
    pipe->file_descriptor = os_channel_get_fd(pipe->channel);
    ring_id_t transmit_id = os_channel_ring_id(
        pipe->channel, CHANNEL_FIRST_TX_RING);
    ring_id_t receive_id = os_channel_ring_id(
        pipe->channel, CHANNEL_FIRST_RX_RING);
    pipe->transmit_ring = os_channel_tx_ring(pipe->channel, transmit_id);
    pipe->receive_ring = os_channel_rx_ring(pipe->channel, receive_id);
    if (pipe->file_descriptor < 0 || pipe->transmit_ring == NULL ||
        pipe->receive_ring == NULL) {
        NativeReleasePipe(pipe);
        return -ENODEV;
    }
    pipe->transmit_kqueue = NativeRegisterKqueue(
        pipe->file_descriptor, EVFILT_WRITE);
    if (pipe->transmit_kqueue < 0) {
        int error = pipe->transmit_kqueue;
        NativeReleasePipe(pipe);
        return error;
    }
    pipe->receive_kqueue = NativeRegisterKqueue(
        pipe->file_descriptor, EVFILT_READ);
    if (pipe->receive_kqueue < 0) {
        int error = pipe->receive_kqueue;
        NativeReleasePipe(pipe);
        return error;
    }
    return 0;
}

static int NativeOpen(void *opaque_context) {
    static const char *protocols[SKYWALK_HCI_PIPE_COUNT] = {
        "hci", "acl", "sco", "iso"
    };
    NativeDriverContext *context = opaque_context;
    if (context == NULL) return -EINVAL;
    native_debug_send_count = 0;
    native_debug_late_receive_count = 0;
    NativeReleasePipes(context);
    atomic_store_explicit(&context->cancel_open, false, memory_order_release);
    context->open_wait_reported = false;
    (void)pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);

    for (int index = 0; index < SKYWALK_HCI_PIPE_COUNT; ++index) {
        int error = CopySkywalkUUIDForProtocol(
            protocols[index], context->pipe_uuids[index]);
        if (error != 0) return -error;
    }

    uint32_t timeout_ms = atomic_load_explicit(&context->open_timeout_ms,
                                               memory_order_acquire);
    uint64_t now_ms = MonotonicMilliseconds();
    uint64_t deadline_ms = timeout_ms == 0 || now_ms == 0
        ? 0
        : now_ms + timeout_ms;

    for (int index = 0; index < SKYWALK_HCI_PIPE_COUNT; ++index) {
        int error = NativeOpenPipe(
            context, (SkywalkHCIPipe)index, deadline_ms);
        if (error != 0) {
            NativeReleasePipes(context);
            return error;
        }
    }
    return 0;
}

static int NativeFileDescriptor(void *opaque_context, SkywalkHCIPipe pipe) {
    NativeDriverContext *context = opaque_context;
    if (context == NULL || pipe < 0 || pipe >= SKYWALK_HCI_PIPE_COUNT) {
        return -EINVAL;
    }
    return context->pipes[pipe].file_descriptor;
}

static int NativeCanSendPacketNow(void *opaque_context) {
    NativeDriverContext *context = opaque_context;
    return context != NULL;
}

static int NativePollTransmit(void *opaque_context) {
    NativeDriverContext *context = opaque_context;
    return context == NULL ? -EINVAL : 0;
}

static int NativeAdvanceAndSyncTransmit(NativePipe *pipe,
                                        channel_slot_t slot) {
    if (pipe == NULL || pipe->channel == NULL || slot == NULL) {
        return -EINVAL;
    }
    errno = 0;
    int error = NativeStatus(
        os_channel_advance_slot(pipe->transmit_ring, slot));
    if (error != 0) return error;
    errno = 0;
    return NativeStatus(os_channel_sync(pipe->channel, CHANNEL_SYNC_TX));
}

static uint16_t ReadLittleEndian16(const uint8_t *bytes) {
    return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

int SkywalkHCIPacketLength(SkywalkHCIPipe pipe,
                          const uint8_t *slot,
                          size_t slot_size) {
    if (slot == NULL || pipe < 0 || pipe >= SKYWALK_HCI_PIPE_COUNT) {
        return -EINVAL;
    }

    size_t header_size;
    size_t payload_size;
    switch (pipe) {
        case SKYWALK_HCI_PIPE_HCI:
            header_size = 2;
            if (slot_size < header_size) return -EPROTO;
            payload_size = slot[1];
            break;
        case SKYWALK_HCI_PIPE_ACL:
            header_size = 4;
            if (slot_size < header_size) return -EPROTO;
            payload_size = ReadLittleEndian16(&slot[2]);
            break;
        case SKYWALK_HCI_PIPE_SCO:
            header_size = 3;
            if (slot_size < header_size) return -EPROTO;
            payload_size = slot[2];
            break;
        case SKYWALK_HCI_PIPE_ISO:
            header_size = 4;
            if (slot_size < header_size) return -EPROTO;
            payload_size = ReadLittleEndian16(&slot[2]) & 0x3fff;
            break;
        default:
            return -EINVAL;
    }

    size_t packet_size = header_size + payload_size;
    if (packet_size > (size_t)INT_MAX) return -EOVERFLOW;
    if (slot_size == packet_size || slot_size == packet_size + 8) {
        return (int)packet_size;
    }
    return -EPROTO;
}

static int NativeSendPacket(void *opaque_context,
                            SkywalkHCIPipe pipe_kind,
                            const uint8_t *packet,
                            size_t size) {
    NativeDriverContext *context = opaque_context;
    if (context == NULL || packet == NULL ||
        pipe_kind < 0 || pipe_kind >= SKYWALK_HCI_PIPE_COUNT) {
        return -EINVAL;
    }
    if (size > UINT16_MAX) return -EMSGSIZE;
    NativePipe *pipe = &context->pipes[pipe_kind];
    if (pipe->channel == NULL || pipe->transmit_ring == NULL) return -ENODEV;
    uint32_t available_before =
        os_channel_available_slot_count(pipe->transmit_ring);
    slot_prop_t properties;
    memset(&properties, 0, sizeof(properties));
    channel_slot_t slot = os_channel_get_next_slot(pipe->transmit_ring,
                                                   NULL,
                                                   &properties);
    if (slot == NULL) return -EAGAIN;
    unsigned int debug_index = native_debug_send_count++;
    if (debug_index < 40) {
        dprintf(STDERR_FILENO,
                "native tx n=%u pipe=%d avail=%u slot=%u capacity=%u size=%zu\n",
                debug_index + 1,
                pipe_kind,
                available_before,
                properties.index,
                properties.length,
                size);
    }
    if (properties.buffer == 0 || size > properties.length) {
        return -EMSGSIZE;
    }
    memcpy((void *)(uintptr_t)properties.buffer, packet, size);
    properties.length = (uint16_t)size;
    os_channel_set_slot_properties(pipe->transmit_ring, slot, &properties);
    int error = NativeAdvanceAndSyncTransmit(pipe, slot);
    if (debug_index < 40 || error != 0) {
        dprintf(STDERR_FILENO,
                "native tx done n=%u advance=0 sync=%d avail=%u\n",
                debug_index + 1,
                error,
                os_channel_available_slot_count(pipe->transmit_ring));
    }
    if (error != 0) {
        dprintf(STDERR_FILENO,
                "native tx state pipe=%d defunct=%d pending=%d "
                "sync_time=%llu notify_time=%llu\n",
                pipe_kind,
                os_channel_is_defunct(pipe->channel),
                os_channel_pending(pipe->transmit_ring),
                (unsigned long long)os_channel_ring_sync_time(
                    pipe->transmit_ring),
                (unsigned long long)os_channel_ring_notify_time(
                    pipe->transmit_ring));
    }
    return error;
}

static int NativeReceivePacket(void *opaque_context,
                               SkywalkHCIPipe pipe_kind,
                               uint8_t *packet,
                               size_t capacity,
                               size_t *size) {
    NativeDriverContext *context = opaque_context;
    if (context == NULL || packet == NULL || size == NULL ||
        pipe_kind < 0 || pipe_kind >= SKYWALK_HCI_PIPE_COUNT) {
        return -EINVAL;
    }
    *size = 0;
    NativePipe *pipe = &context->pipes[pipe_kind];
    if (pipe->channel == NULL || pipe->receive_ring == NULL) return -ENODEV;
    /* Consume the same EVFILT_READ event bluetoothd uses.  The ring itself is
     * shared memory, so check it after consuming the event and sync only after
     * advancing received slots. */
    (void)NativeWaitForKqueueEvent(pipe->receive_kqueue, EVFILT_READ, 0);
    uint32_t available =
        os_channel_available_slot_count(pipe->receive_ring);
    if (pipe_kind == SKYWALK_HCI_PIPE_HCI &&
        native_debug_send_count >= 10 &&
        native_debug_late_receive_count++ < 40) {
        dprintf(STDERR_FILENO,
                "native late rx sync=%d errno=%d avail=%u\n",
                0,
                errno,
                available);
    }
    if (available == 0) return 0;

    slot_prop_t properties;
    memset(&properties, 0, sizeof(properties));
    channel_slot_t slot = os_channel_get_next_slot(pipe->receive_ring,
                                                   NULL,
                                                   &properties);
    if (slot == NULL) return 0;

    int packet_size = -EPROTO;
    if (properties.buffer != 0) {
        packet_size = SkywalkHCIPacketLength(
            pipe_kind,
            (const uint8_t *)(uintptr_t)properties.buffer,
            properties.length);
        if (packet_size >= 0 && (size_t)packet_size > capacity) {
            packet_size = -EMSGSIZE;
        }
        if (packet_size >= 0) {
            memcpy(packet,
                   (const void *)(uintptr_t)properties.buffer,
                   (size_t)packet_size);
            *size = (size_t)packet_size;
        }
    }

    errno = 0;
    int error = NativeStatus(
        os_channel_advance_slot(pipe->receive_ring, slot));
    if (error == 0) {
        int release_result = NativeSyncChannel(pipe, CHANNEL_SYNC_RX);
        if (release_result != 0 && release_result != -EAGAIN) {
            error = release_result;
        }
    }
    if (error != 0) return error;
    return packet_size < 0 ? packet_size : 1;
}

const SkywalkHCIDriver *SkywalkHCINativeDriver(void) {
    static const SkywalkHCIDriver driver = {
        .context = &native_context,
        .open = NativeOpen,
        .close = NativeClose,
        .file_descriptor = NativeFileDescriptor,
        .can_send_packet_now = NativeCanSendPacketNow,
        .send_packet = NativeSendPacket,
        .poll_transmit = NativePollTransmit,
        .receive_packet = NativeReceivePacket,
    };
    static bool initialized;
    if (!initialized) {
        atomic_init(&native_context.cancel_open, false);
        atomic_init(&native_context.open_timeout_ms, 120000);
        for (int index = 0; index < SKYWALK_HCI_PIPE_COUNT; ++index) {
            native_context.pipes[index].file_descriptor = -1;
            native_context.pipes[index].transmit_kqueue = -1;
            native_context.pipes[index].receive_kqueue = -1;
        }
        initialized = true;
    }
    return &driver;
}

void SkywalkHCINativeDriverSetOpenTimeoutMilliseconds(uint32_t timeout_ms) {
    (void)SkywalkHCINativeDriver();
    atomic_store_explicit(&native_context.open_timeout_ms,
                          timeout_ms,
                          memory_order_release);
}

void SkywalkHCINativeDriverSetOpenWaitCallback(
    SkywalkHCIOpenWaitCallback callback,
    void *context) {
    (void)SkywalkHCINativeDriver();
    native_context.open_wait_callback = callback;
    native_context.open_wait_context = context;
}

void SkywalkHCINativeDriverCancelOpen(void) {
    (void)SkywalkHCINativeDriver();
    atomic_store_explicit(&native_context.cancel_open,
                          true,
                          memory_order_release);
}
