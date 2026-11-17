/*
 * Copyright (C) 2014 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL BLUEKITCHEN
 * GMBH OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at
 * contact@bluekitchen-gmbh.com
 */

#include "hci_transport_skywalk.h"

#include "bluetooth.h"
#include "btstack_run_loop.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef HCI_TRANSPORT_SKYWALK_TESTING
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#endif

#define SKYWALK_HCI_RECEIVE_BUFFER_SIZE 4096
#define SKYWALK_HCI_MAX_PACKETS_PER_WAKE 64

typedef struct {
    btstack_data_source_t data_source;
    SkywalkHCIPipe pipe;
    uint8_t receive_buffer[SKYWALK_HCI_RECEIVE_BUFFER_SIZE];
} SkywalkHCIDataSource;

static const SkywalkHCIDriver *skywalk_driver;
static SkywalkHCIDataSource skywalk_sources[SKYWALK_HCI_PIPE_COUNT];
static btstack_data_source_t skywalk_wake_source;
static int skywalk_wake_pipe[2] = {-1, -1};
static pthread_mutex_t skywalk_wake_pipe_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t skywalk_poll_thread;
static bool skywalk_poll_thread_created;
static _Atomic bool skywalk_poll_thread_running;
static _Atomic int skywalk_external_fault;
static bool skywalk_open;
static bool skywalk_transmit_pending;
static bool skywalk_pending_needs_slot;
static bool skywalk_packet_sent_event_pending;
static uint8_t skywalk_pending_packet_type;
static uint8_t *skywalk_pending_packet;
static int skywalk_pending_size;
static unsigned int debug_wake_count;
static unsigned int debug_send_count;
static unsigned int debug_receive_count;
static uint64_t skywalk_transmit_started_ms;
static bool skywalk_fault_reported;
static SkywalkHCITransportFaultHandler skywalk_fault_handler;
static void *skywalk_fault_context;
#ifdef HCI_TRANSPORT_SKYWALK_TESTING
static uint64_t skywalk_now_ms_for_testing;
#endif

enum {
    SKYWALK_HCI_TRANSMIT_TIMEOUT_MS = 1500,
    SKYWALK_ACT_EVENT_FATAL = 0,
    SKYWALK_ACT_EVENT_CONTROLLER_RESET = 2,
};

static void WakeSkywalkRunLoop(void) {
    pthread_mutex_lock(&skywalk_wake_pipe_mutex);
    if (skywalk_wake_pipe[1] >= 0) {
        uint8_t wake = 1;
        (void)write(skywalk_wake_pipe[1], &wake, sizeof(wake));
    }
    pthread_mutex_unlock(&skywalk_wake_pipe_mutex);
}

static void HandleAppleConvergedTransportEvent(uint32_t type,
                                                const char *message,
                                                uint32_t value) {
    if (type != SKYWALK_ACT_EVENT_FATAL &&
        type != SKYWALK_ACT_EVENT_CONTROLLER_RESET) return;

    dprintf(STDERR_FILENO,
            "Apple Bluetooth transport event type=%u value=%u message=%s\n",
            type,
            value,
            message != NULL ? message : "");
    atomic_store_explicit(&skywalk_external_fault,
                          -EHOSTDOWN,
                          memory_order_release);
    WakeSkywalkRunLoop();
}

#ifdef HCI_TRANSPORT_SKYWALK_TESTING
static int RegisterAppleConvergedTransportListener(void) {
    return 0;
}

static void UnregisterAppleConvergedTransportListener(void) {}
#else
typedef void (^AppleConvergedTransportEventBlock)(uint32_t type,
                                                   const char *message,
                                                   uint32_t value);
typedef bool (*AppleConvergedTransportRegisterEventBlockQ)(
    AppleConvergedTransportEventBlock block,
    dispatch_queue_t queue);
typedef bool (*AppleConvergedTransportUnregisterEventBlock)(void);

static void *skywalk_act_library;
static dispatch_queue_t skywalk_act_event_queue;
static AppleConvergedTransportRegisterEventBlockQ skywalk_act_register;
static AppleConvergedTransportUnregisterEventBlock skywalk_act_unregister;
static bool skywalk_act_registered;

static AppleConvergedTransportEventBlock skywalk_act_event_block =
    ^(uint32_t type, const char *message, uint32_t value) {
        HandleAppleConvergedTransportEvent(type, message, value);
    };

static int LoadAppleConvergedTransportEventAPI(void) {
    if (skywalk_act_register != NULL && skywalk_act_unregister != NULL) {
        return 0;
    }

    if (skywalk_act_library == NULL) {
        skywalk_act_library = dlopen(
            "/usr/lib/AppleConvergedTransport.dylib",
            RTLD_NOW | RTLD_LOCAL);
    }
    if (skywalk_act_library == NULL) {
        dprintf(STDERR_FILENO,
                "Could not load Apple Bluetooth transport events: %s\n",
                dlerror());
        return -ENOSYS;
    }

    skywalk_act_register =
        (AppleConvergedTransportRegisterEventBlockQ)dlsym(
            skywalk_act_library,
            "AppleConvergedTransportRegisterEventBlockQ");
    skywalk_act_unregister =
        (AppleConvergedTransportUnregisterEventBlock)dlsym(
            skywalk_act_library,
            "AppleConvergedTransportUnregisterEventBlock");
    if (skywalk_act_register == NULL || skywalk_act_unregister == NULL) {
        dprintf(STDERR_FILENO,
                "Apple Bluetooth transport event functions are missing\n");
        return -ENOSYS;
    }

    if (skywalk_act_event_queue == NULL) {
        skywalk_act_event_queue = dispatch_queue_create(
            "AndroidMacCallBridge.AppleBluetoothEvents",
            DISPATCH_QUEUE_SERIAL);
    }
    return skywalk_act_event_queue != NULL ? 0 : -ENOMEM;
}

static int RegisterAppleConvergedTransportListener(void) {
    if (skywalk_act_registered) return 0;
    int result = LoadAppleConvergedTransportEventAPI();
    if (result != 0) return result;
    if (!skywalk_act_register(skywalk_act_event_block,
                              skywalk_act_event_queue)) {
        dprintf(STDERR_FILENO,
                "Could not register Apple Bluetooth transport events\n");
        return -EBUSY;
    }
    skywalk_act_registered = true;
    return 0;
}

static void UnregisterAppleConvergedTransportListener(void) {
    if (!skywalk_act_registered) return;
    if (!skywalk_act_unregister()) {
        dprintf(STDERR_FILENO,
                "Could not unregister Apple Bluetooth transport events\n");
    }
    skywalk_act_registered = false;
}
#endif

static uint64_t MonotonicMilliseconds(void) {
#ifdef HCI_TRANSPORT_SKYWALK_TESTING
    if (skywalk_now_ms_for_testing != 0) return skywalk_now_ms_for_testing;
#endif
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * UINT64_C(1000) +
        (uint64_t)now.tv_nsec / UINT64_C(1000000);
}

static void ReportTransportFault(int error) {
    if (skywalk_fault_reported) return;
    skywalk_fault_reported = true;
    dprintf(STDERR_FILENO,
            "skywalk transport fault error=%d\n",
            error);
    if (skywalk_fault_handler != NULL) {
        skywalk_fault_handler(skywalk_fault_context, error);
    }
}

static void ClearPendingTransmit(void) {
    skywalk_transmit_pending = false;
    skywalk_pending_needs_slot = false;
    skywalk_packet_sent_event_pending = false;
    skywalk_pending_packet = NULL;
    skywalk_pending_size = 0;
    skywalk_transmit_started_ms = 0;
}

static bool TransmitTimedOut(void) {
    if (!skywalk_transmit_pending || skywalk_transmit_started_ms == 0) {
        return false;
    }
    uint64_t now_ms = MonotonicMilliseconds();
    return now_ms != 0 && now_ms >= skywalk_transmit_started_ms &&
        now_ms - skywalk_transmit_started_ms >=
            SKYWALK_HCI_TRANSMIT_TIMEOUT_MS;
}

static void DummyPacketHandler(uint8_t packet_type,
                               uint8_t *packet,
                               uint16_t size) {
    (void)packet_type;
    (void)packet;
    (void)size;
}

static void (*skywalk_packet_handler)(uint8_t packet_type,
                                      uint8_t *packet,
                                      uint16_t size) = DummyPacketHandler;

static bool PipeForPacketType(uint8_t packet_type, SkywalkHCIPipe *pipe);
static int SkywalkClose(void);

static void EmitTransportPacketSent(void) {
    uint8_t event[] = {HCI_EVENT_TRANSPORT_PACKET_SENT, 0};
    skywalk_packet_handler(HCI_EVENT_PACKET, event, sizeof(event));
}

static void ProcessTransmit(void) {
    if (!skywalk_open || skywalk_driver == NULL) return;

    int external_fault = atomic_load_explicit(&skywalk_external_fault,
                                              memory_order_acquire);
    if (external_fault != 0) {
        ClearPendingTransmit();
        ReportTransportFault(external_fault);
        return;
    }

    if (skywalk_transmit_pending && skywalk_pending_needs_slot) {
        SkywalkHCIPipe pipe;
        if (!PipeForPacketType(skywalk_pending_packet_type, &pipe)) return;
        int result = skywalk_driver->send_packet(
            skywalk_driver->context,
            pipe,
            skywalk_pending_packet,
            (size_t)skywalk_pending_size);
        if (result == SKYWALK_HCI_SEND_COMPLETE) {
            skywalk_transmit_pending = false;
            skywalk_pending_needs_slot = false;
            skywalk_transmit_started_ms = 0;
            skywalk_packet_sent_event_pending = true;
        } else if (result == SKYWALK_HCI_SEND_PENDING) {
            skywalk_pending_needs_slot = false;
        } else if (result != -EAGAIN) {
            dprintf(STDERR_FILENO,
                    "skywalk queued tx failed result=%d\n",
                    result);
            ClearPendingTransmit();
            ReportTransportFault(result);
            return;
        }
    }

    if (skywalk_transmit_pending && !skywalk_pending_needs_slot &&
        skywalk_driver->poll_transmit != NULL) {
        int result = skywalk_driver->poll_transmit(skywalk_driver->context);
        if (result > 0) {
            skywalk_transmit_pending = false;
            skywalk_transmit_started_ms = 0;
            skywalk_packet_sent_event_pending = true;
        } else if (result < 0) {
            dprintf(STDERR_FILENO,
                    "skywalk pending tx failed result=%d\n",
                    result);
            ClearPendingTransmit();
            ReportTransportFault(result);
            return;
        }
    }

    /* A send can wait either for a free Skywalk slot or for an accepted
     * asynchronous send to finish.  Both states use the same deadline, even
     * when the driver has no poll callback. */
    if (TransmitTimedOut()) {
        ClearPendingTransmit();
        ReportTransportFault(-ETIMEDOUT);
        return;
    }

    if (skywalk_packet_sent_event_pending) {
        skywalk_packet_sent_event_pending = false;
        skywalk_pending_packet = NULL;
        skywalk_pending_size = 0;
        EmitTransportPacketSent();
    }
}

static uint8_t PacketTypeForPipe(SkywalkHCIPipe pipe) {
    static const uint8_t packet_types[SKYWALK_HCI_PIPE_COUNT] = {
        HCI_EVENT_PACKET,
        HCI_ACL_DATA_PACKET,
        HCI_SCO_DATA_PACKET,
        HCI_ISO_DATA_PACKET,
    };
    if (pipe < 0 || pipe >= SKYWALK_HCI_PIPE_COUNT) return 0;
    return packet_types[pipe];
}

static bool PipeForPacketType(uint8_t packet_type, SkywalkHCIPipe *pipe) {
    if (pipe == NULL) return false;
    switch (packet_type) {
        case HCI_COMMAND_DATA_PACKET:
            *pipe = SKYWALK_HCI_PIPE_HCI;
            return true;
        case HCI_ACL_DATA_PACKET:
            *pipe = SKYWALK_HCI_PIPE_ACL;
            return true;
        case HCI_SCO_DATA_PACKET:
            *pipe = SKYWALK_HCI_PIPE_SCO;
            return true;
        case HCI_ISO_DATA_PACKET:
            *pipe = SKYWALK_HCI_PIPE_ISO;
            return true;
        default:
            return false;
    }
}

static void ProcessPipe(SkywalkHCIPipe pipe) {
    if (!skywalk_open || skywalk_driver == NULL ||
        skywalk_driver->receive_packet == NULL) return;
    if (pipe < 0 || pipe >= SKYWALK_HCI_PIPE_COUNT) return;

    SkywalkHCIDataSource *source = &skywalk_sources[pipe];
    for (unsigned int count = 0;
         count < SKYWALK_HCI_MAX_PACKETS_PER_WAKE;
         ++count) {
        size_t size = 0;
        int result = skywalk_driver->receive_packet(
            skywalk_driver->context,
            pipe,
            source->receive_buffer,
            sizeof(source->receive_buffer),
            &size);
        if (result < 0) {
            if (debug_receive_count++ < 40) {
                dprintf(STDERR_FILENO,
                        "skywalk rx pipe=%d error=%d\n",
                        pipe,
                        result);
            }
            return;
        }
        if (result == 0) return;
        if (debug_receive_count++ < 40) {
            dprintf(STDERR_FILENO,
                    "skywalk rx pipe=%d size=%zu first=%02x %02x %02x\n",
                    pipe,
                    size,
                    size > 0 ? source->receive_buffer[0] : 0,
                    size > 1 ? source->receive_buffer[1] : 0,
                    size > 2 ? source->receive_buffer[2] : 0);
        }
        if (size == 0 || size > UINT16_MAX) return;
        skywalk_packet_handler(PacketTypeForPipe(pipe),
                               source->receive_buffer,
                               (uint16_t)size);
    }
}

static void ProcessDataSource(
    btstack_data_source_t *data_source,
    btstack_data_source_callback_type_t callback_type) {
    if (callback_type != DATA_SOURCE_CALLBACK_READ || data_source == NULL) {
        return;
    }
    uint8_t bytes[64];
    while (read(skywalk_wake_pipe[0], bytes, sizeof(bytes)) > 0) {}
    int external_fault = atomic_load_explicit(&skywalk_external_fault,
                                              memory_order_acquire);
    if (external_fault != 0) {
        ClearPendingTransmit();
        ReportTransportFault(external_fault);
        return;
    }
    if (debug_wake_count++ < 20) {
        dprintf(STDERR_FILENO, "skywalk main wake=%u\n", debug_wake_count);
    }
    for (int index = 0; index < SKYWALK_HCI_PIPE_COUNT; ++index) {
        ProcessPipe((SkywalkHCIPipe)index);
    }
    ProcessTransmit();
}

static void *PollSkywalkPipes(void *context) {
    (void)context;
    while (atomic_load_explicit(&skywalk_poll_thread_running,
                                memory_order_acquire)) {
        /* This helper only wakes BTstack's select loop.  ProcessPipe checks
         * its read kqueue before draining the Skywalk receive ring. */
        (void)poll(NULL, 0, 2);
        if (!atomic_load_explicit(&skywalk_poll_thread_running,
                                  memory_order_acquire)) break;
        WakeSkywalkRunLoop();
    }
    return NULL;
}

static void SkywalkInit(const void *transport_config) {
    (void)transport_config;
}

static int SkywalkOpen(void) {
    if (skywalk_open) return 0;
    if (skywalk_driver == NULL || skywalk_driver->open == NULL ||
        skywalk_driver->close == NULL ||
        skywalk_driver->file_descriptor == NULL ||
        skywalk_driver->send_packet == NULL ||
        skywalk_driver->receive_packet == NULL) return -EINVAL;

    atomic_store_explicit(&skywalk_external_fault, 0, memory_order_release);
    int result = skywalk_driver->open(skywalk_driver->context);
    if (result != 0) return result < 0 ? result : -result;

    memset(skywalk_sources, 0, sizeof(skywalk_sources));
    for (int index = 0; index < SKYWALK_HCI_PIPE_COUNT; ++index) {
        SkywalkHCIDataSource *source = &skywalk_sources[index];
        source->pipe = (SkywalkHCIPipe)index;
        int file_descriptor = skywalk_driver->file_descriptor(
            skywalk_driver->context, source->pipe);
        if (file_descriptor < 0) {
            for (int added = 0; added < index; ++added) {
                btstack_run_loop_disable_data_source_callbacks(
                    &skywalk_sources[added].data_source,
                    DATA_SOURCE_CALLBACK_READ);
                btstack_run_loop_remove_data_source(
                    &skywalk_sources[added].data_source);
            }
            skywalk_driver->close(skywalk_driver->context);
            return -ENODEV;
        }
        (void)file_descriptor;
    }
    if (pipe(skywalk_wake_pipe) != 0) {
        skywalk_driver->close(skywalk_driver->context);
        return -errno;
    }
    (void)fcntl(skywalk_wake_pipe[0], F_SETFL, O_NONBLOCK);
    (void)fcntl(skywalk_wake_pipe[1], F_SETFL, O_NONBLOCK);
    memset(&skywalk_wake_source, 0, sizeof(skywalk_wake_source));
    btstack_run_loop_set_data_source_handler(&skywalk_wake_source,
                                              ProcessDataSource);
    btstack_run_loop_set_data_source_fd(&skywalk_wake_source,
                                        skywalk_wake_pipe[0]);
    btstack_run_loop_add_data_source(&skywalk_wake_source);
    btstack_run_loop_enable_data_source_callbacks(&skywalk_wake_source,
                                                   DATA_SOURCE_CALLBACK_READ);
    atomic_store_explicit(&skywalk_poll_thread_running,
                          true,
                          memory_order_release);
    int thread_error = pthread_create(&skywalk_poll_thread,
                                      NULL,
                                      PollSkywalkPipes,
                                      NULL);
    if (thread_error != 0) {
        atomic_store(&skywalk_poll_thread_running, false);
        btstack_run_loop_remove_data_source(&skywalk_wake_source);
        close(skywalk_wake_pipe[0]);
        close(skywalk_wake_pipe[1]);
        skywalk_wake_pipe[0] = -1;
        skywalk_wake_pipe[1] = -1;
        skywalk_driver->close(skywalk_driver->context);
        return -thread_error;
    }
    skywalk_poll_thread_created = true;
    skywalk_transmit_pending = false;
    skywalk_pending_needs_slot = false;
    skywalk_packet_sent_event_pending = false;
    skywalk_pending_packet = NULL;
    skywalk_pending_size = 0;
    skywalk_transmit_started_ms = 0;
    skywalk_fault_reported = false;
    skywalk_open = true;
    int listener_result = RegisterAppleConvergedTransportListener();
    if (listener_result != 0) {
        (void)SkywalkClose();
        return listener_result;
    }
    dprintf(STDERR_FILENO, "skywalk transport opened\n");
    return 0;
}

static int SkywalkClose(void) {
    if (!skywalk_open) return 0;
    skywalk_open = false;
    ClearPendingTransmit();
    UnregisterAppleConvergedTransportListener();
    atomic_store_explicit(&skywalk_poll_thread_running,
                          false,
                          memory_order_release);
    if (skywalk_poll_thread_created) {
        pthread_join(skywalk_poll_thread, NULL);
        skywalk_poll_thread_created = false;
    }
    btstack_run_loop_disable_data_source_callbacks(&skywalk_wake_source,
                                                    DATA_SOURCE_CALLBACK_READ);
    btstack_run_loop_remove_data_source(&skywalk_wake_source);
    pthread_mutex_lock(&skywalk_wake_pipe_mutex);
    if (skywalk_wake_pipe[0] >= 0) close(skywalk_wake_pipe[0]);
    if (skywalk_wake_pipe[1] >= 0) close(skywalk_wake_pipe[1]);
    skywalk_wake_pipe[0] = -1;
    skywalk_wake_pipe[1] = -1;
    pthread_mutex_unlock(&skywalk_wake_pipe_mutex);
    if (skywalk_driver != NULL) {
        skywalk_driver->close(skywalk_driver->context);
    }
    memset(skywalk_sources, 0, sizeof(skywalk_sources));
    return 0;
}

static void SkywalkRegisterPacketHandler(
    void (*handler)(uint8_t packet_type, uint8_t *packet, uint16_t size)) {
    skywalk_packet_handler = handler != NULL ? handler : DummyPacketHandler;
}

static int SkywalkSendPacket(uint8_t packet_type,
                             uint8_t *packet,
                             int size) {
    if (!skywalk_open || packet == NULL || size < 0) return -EINVAL;
    int external_fault = atomic_load_explicit(&skywalk_external_fault,
                                              memory_order_acquire);
    if (external_fault != 0) {
        ClearPendingTransmit();
        ReportTransportFault(external_fault);
        return external_fault;
    }
    if (skywalk_transmit_pending || skywalk_packet_sent_event_pending) {
        return -EAGAIN;
    }
    SkywalkHCIPipe pipe;
    if (!PipeForPacketType(packet_type, &pipe)) return -EINVAL;
    int result = skywalk_driver->send_packet(skywalk_driver->context,
                                             pipe,
                                             packet,
                                             (size_t)size);
    unsigned int debug_index = debug_send_count++;
    if (debug_index < 40 || result != 0) {
        dprintf(STDERR_FILENO,
                "skywalk tx type=%u pipe=%d size=%d result=%d first=%02x %02x %02x\n",
                packet_type,
                pipe,
                size,
                result,
                size > 0 ? packet[0] : 0,
                size > 1 ? packet[1] : 0,
                size > 2 ? packet[2] : 0);
    }
    if (result < 0 && result != -EAGAIN) {
        ClearPendingTransmit();
        ReportTransportFault(result);
        return result;
    }
    skywalk_transmit_pending = result != SKYWALK_HCI_SEND_COMPLETE;
    skywalk_pending_needs_slot = result == -EAGAIN;
    skywalk_packet_sent_event_pending =
        result == SKYWALK_HCI_SEND_COMPLETE;
    skywalk_pending_packet_type = packet_type;
    skywalk_pending_packet = packet;
    skywalk_pending_size = size;
    if (skywalk_transmit_pending && skywalk_transmit_started_ms == 0) {
        skywalk_transmit_started_ms = MonotonicMilliseconds();
    } else if (!skywalk_transmit_pending) {
        skywalk_transmit_started_ms = 0;
    }
    return 0;
}

static int SkywalkCanSendPacketNow(uint8_t packet_type) {
    (void)packet_type;
    if (!skywalk_open || skywalk_driver == NULL) return 0;
    if (atomic_load_explicit(&skywalk_external_fault,
                             memory_order_acquire) != 0) return 0;
    if (skywalk_transmit_pending || skywalk_packet_sent_event_pending) return 0;
    if (skywalk_driver->can_send_packet_now == NULL) return 1;
    return skywalk_driver->can_send_packet_now(skywalk_driver->context);
}

static const hci_transport_t skywalk_transport = {
    .name = "macOS Skywalk",
    .init = SkywalkInit,
    .open = SkywalkOpen,
    .close = SkywalkClose,
    .register_packet_handler = SkywalkRegisterPacketHandler,
    .can_send_packet_now = SkywalkCanSendPacketNow,
    .send_packet = SkywalkSendPacket,
    .set_baudrate = NULL,
    .reset_link = NULL,
    .set_sco_config = NULL,
};

const hci_transport_t *hci_transport_skywalk_instance(
    const SkywalkHCIDriver *driver) {
    if (!skywalk_open) skywalk_driver = driver;
    return &skywalk_transport;
}

void hci_transport_skywalk_set_fault_handler(
    SkywalkHCITransportFaultHandler handler,
    void *context) {
    skywalk_fault_handler = handler;
    skywalk_fault_context = context;
}

#ifdef HCI_TRANSPORT_SKYWALK_TESTING
void hci_transport_skywalk_process_pipe_for_testing(SkywalkHCIPipe pipe) {
    ProcessPipe(pipe);
}

void hci_transport_skywalk_process_transmit_for_testing(void) {
    ProcessTransmit();
}

void hci_transport_skywalk_set_now_for_testing(uint64_t now_ms) {
    skywalk_now_ms_for_testing = now_ms;
}

void hci_transport_skywalk_act_event_for_testing(uint32_t type,
                                                  const char *message,
                                                  uint32_t value) {
    HandleAppleConvergedTransportEvent(type, message, value);
}
#endif
