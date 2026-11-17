// panhost.c — Bluetooth tethering client (PAN user over BNEP) for macOS.
//
// Runs BTstack directly on the Mac's built-in Bluetooth chip through its Skywalk HCI
// channels (src/chip), so Apple's 1024-byte limit on classic L2CAP channels does not
// apply. Connects to the phone's Network Access Point and bridges Ethernet frames to a
// BPF handle on one end of a feth pair.
//
//   panhost --phone AA:BB:CC:DD:EE:FF --local-address A2:BB:CC:DD:EE:01 \
//           --state-db <file> [--netif feth1]
//
// Without --netif it runs a self-test: opens BNEP, sends one DHCP DISCOVER,
// prints the phone's OFFER and exits. No interface and no root needed for that.
//
// Apple's Bluetooth must already be powered off by the caller (see bt-tether).

#define _DARWIN_C_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/bpf.h>
#include <net/if.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>

#include "BroadcomAddressTest.h"
#include "hci_transport_skywalk.h"

#include "btstack_config.h"
#include "bluetooth.h"
#include "bluetooth_sdp.h"
#include "btstack_debug.h"
#include "btstack_defines.h"
#include "btstack_event.h"
#include "btstack_memory.h"
#include "btstack_run_loop.h"
#include "btstack_tlv.h"
#include "btstack_util.h"
#include "gap.h"
#include "hci.h"
#include "hci_cmd.h"
#include "hci_dump.h"
#include "l2cap.h"
#include "classic/bnep.h"
#include "classic/btstack_link_key_db_tlv.h"
#include "classic/pan.h"
#include "classic/sdp_server.h"
#include "classic/sdp_util.h"
#include "btstack_run_loop_posix.h"
#include "btstack_tlv_posix.h"
#include "hci_dump_posix_stdout.h"

#define ETH_IPV4 0x0800
#define ETH_ARP  0x0806
#define ETH_IPV6 0x86DD
#define BNEP_PSM 0x000F
#define MAX_FRAME 1514
#define TXQ_CAP 256

typedef struct {
    bd_addr_t phone;
    bd_addr_t local;
    char phone_str[18];
    char local_str[18];
    const char *netif;
    bool probe_mode;

    uint16_t bnep_cid;
    bool bnep_open;
    bool connecting;
    unsigned connect_attempts;

    btstack_tlv_posix_t tlv_context;
    btstack_timer_source_t tick_timer;
    btstack_timer_source_t reconnect_timer;
    btstack_data_source_t stdin_source;
    btstack_data_source_t bpf_source;

    int bpf_fd;
    uint8_t *bpf_buf;
    size_t bpf_buflen;

    uint8_t txq[TXQ_CAP][MAX_FRAME + 2];
    uint16_t txq_len[TXQ_CAP];
    unsigned txq_head, txq_count;

    uint64_t to_phone_frames, to_phone_bytes, from_phone_frames, from_phone_bytes, dropped;
    uint64_t last_stats_ms;

    uint32_t xid;
    unsigned probe_sends;
    bool probe_done;

    bool stopping;
    uint64_t stop_deadline_ms;
    int exit_status;
} Program;

static Program program;
static volatile sig_atomic_t stop_requested;

static uint64_t NowMs(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}

static void Emit(const char *fmt, ...) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    time_t secs = tv.tv_sec;
    localtime_r(&secs, &tm);
    printf("%02d:%02d:%02d.%03d ", tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(tv.tv_usec / 1000));
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

// ---------------------------------------------------------------- shutdown

static void FinishAndExit(int status) {
    if (program.bpf_fd >= 0) close(program.bpf_fd);
    btstack_tlv_posix_deinit(&program.tlv_context);
    Emit("event=exit status=%d to_phone=%llu from_phone=%llu dropped=%llu", status,
         (unsigned long long)program.to_phone_frames, (unsigned long long)program.from_phone_frames,
         (unsigned long long)program.dropped);
    exit(status);
}

static void Stop(int status) {
    if (program.stopping) return;
    program.stopping = true;
    program.exit_status = status;
    program.stop_deadline_ms = NowMs() + 4000;
    Emit("event=stopping");
    if (program.bnep_open) bnep_disconnect(program.phone);
    hci_power_control(HCI_POWER_OFF);
}

static void SignalHandler(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

static void TransportFailed(void *context, int error) {
    (void)context;
    Emit("event=transport_fault error=%d", error);
    FinishAndExit(9);
}

// ---------------------------------------------------------------- frames to the phone

static void PumpTx(void) {
    if (!program.bnep_open || program.txq_count == 0) return;
    bnep_request_can_send_now_event(program.bnep_cid);
}

static void QueueToPhone(const uint8_t *frame, size_t len) {
    if (len < 14 || len > MAX_FRAME) return;
    if (!program.bnep_open || program.txq_count == TXQ_CAP) { program.dropped++; return; }
    unsigned slot = (program.txq_head + program.txq_count) % TXQ_CAP;
    memcpy(program.txq[slot], frame, len);
    program.txq_len[slot] = (uint16_t)len;
    program.txq_count++;
    PumpTx();
}

static void SendOneToPhone(void) {
    if (program.txq_count == 0) return;
    unsigned slot = program.txq_head;
    int status = bnep_send(program.bnep_cid, program.txq[slot], program.txq_len[slot]);
    if (status == 0) {
        program.to_phone_frames++;
        program.to_phone_bytes += program.txq_len[slot];
    } else {
        program.dropped++;
    }
    program.txq_head = (program.txq_head + 1) % TXQ_CAP;
    program.txq_count--;
    PumpTx();
}

// ---------------------------------------------------------------- network side (BPF on feth)

static int OpenBpf(const char *ifname) {
    int fd = -1;
    char path[32];
    int last_errno = 0;
    for (int index = 0; index < 256; ++index) {
        snprintf(path, sizeof path, "/dev/bpf%d", index);
        fd = open(path, O_RDWR);
        if (fd >= 0) break;
        last_errno = errno;
        if (errno == ENOENT) break;
    }
    if (fd < 0) {
        Emit("event=bpf_open_failed errno=%d (%s)", last_errno, strerror(last_errno));
        return -1;
    }
    u_int buffer_length = 256 * 1024;
    (void)ioctl(fd, BIOCSBLEN, &buffer_length);
    struct ifreq request;
    memset(&request, 0, sizeof request);
    strlcpy(request.ifr_name, ifname, sizeof request.ifr_name);
    if (ioctl(fd, BIOCSETIF, &request) < 0) {
        Emit("event=bpf_bind_failed if=%s errno=%d (%s)", ifname, errno, strerror(errno));
        close(fd);
        return -1;
    }
    u_int one = 1, zero = 0;
    (void)ioctl(fd, BIOCIMMEDIATE, &one);
    (void)ioctl(fd, BIOCSHDRCMPLT, &one);
    (void)ioctl(fd, BIOCSSEESENT, &zero);
    (void)ioctl(fd, BIOCPROMISC, NULL);
    if (ioctl(fd, BIOCGBLEN, &buffer_length) < 0) buffer_length = 4096;
    program.bpf_buflen = buffer_length;
    program.bpf_buf = malloc(buffer_length);
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    Emit("event=bpf_ready path=%s if=%s buffer=%u", path, ifname, buffer_length);
    return fd;
}

static void ReadBpf(btstack_data_source_t *source, btstack_data_source_callback_type_t type) {
    (void)source;
    (void)type;
    for (int round = 0; round < 4; ++round) {
        ssize_t got = read(program.bpf_fd, program.bpf_buf, program.bpf_buflen);
        if (got <= 0) return;
        uint8_t *cursor = program.bpf_buf;
        uint8_t *end = program.bpf_buf + got;
        while (cursor + sizeof(struct bpf_hdr) <= end) {
            struct bpf_hdr *header = (struct bpf_hdr *)cursor;
            uint8_t *frame = cursor + header->bh_hdrlen;
            size_t length = header->bh_caplen;
            if (frame + length > end) break;
            if (length >= 14 && header->bh_caplen == header->bh_datalen) {
                uint16_t ethertype = (uint16_t)((frame[12] << 8) | frame[13]);
                if (ethertype == ETH_IPV4 || ethertype == ETH_ARP || ethertype == ETH_IPV6) {
                    QueueToPhone(frame, length);
                }
            }
            cursor += BPF_WORDALIGN(header->bh_hdrlen + header->bh_caplen);
        }
    }
}

static void DeliverFromPhone(const uint8_t *frame, uint16_t size) {
    program.from_phone_frames++;
    program.from_phone_bytes += size;
    if (program.bpf_fd < 0) return;
    uint8_t padded[60];
    const uint8_t *out = frame;
    size_t out_len = size;
    if (size < sizeof padded) {
        memset(padded, 0, sizeof padded);
        memcpy(padded, frame, size);
        out = padded;
        out_len = sizeof padded;
    }
    if (write(program.bpf_fd, out, out_len) < 0) program.dropped++;
}

// ---------------------------------------------------------------- self-test (DHCP DISCOVER over BNEP)

static uint16_t IpChecksum(const uint8_t *data, size_t length) {
    uint32_t sum = 0;
    for (size_t index = 0; index + 1 < length; index += 2) sum += (uint32_t)((data[index] << 8) | data[index + 1]);
    if (length & 1) sum += (uint32_t)(data[length - 1] << 8);
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

static void SendDiscover(void) {
    uint8_t frame[14 + 20 + 8 + 300];
    memset(frame, 0, sizeof frame);
    memset(frame, 0xff, 6);
    memcpy(frame + 6, program.local, 6);
    frame[12] = 0x08;
    uint8_t *ip = frame + 14;
    uint16_t ip_len = 20 + 8 + 300;
    ip[0] = 0x45; ip[2] = (uint8_t)(ip_len >> 8); ip[3] = (uint8_t)ip_len; ip[8] = 64; ip[9] = 17;
    memset(ip + 16, 0xff, 4);
    uint16_t checksum = IpChecksum(ip, 20);
    ip[10] = (uint8_t)(checksum >> 8); ip[11] = (uint8_t)checksum;
    uint8_t *udp = ip + 20;
    udp[1] = 68; udp[3] = 67; udp[4] = (8 + 300) >> 8; udp[5] = (8 + 300) & 0xff;
    uint8_t *bootp = udp + 8;
    bootp[0] = 1; bootp[1] = 1; bootp[2] = 6;
    uint32_t xid = htonl(program.xid);
    memcpy(bootp + 4, &xid, 4);
    bootp[10] = 0x80;
    memcpy(bootp + 28, program.local, 6);
    uint8_t *option = bootp + 236;
    *option++ = 0x63; *option++ = 0x82; *option++ = 0x53; *option++ = 0x63;
    *option++ = 53; *option++ = 1; *option++ = 1;
    *option++ = 55; *option++ = 4; *option++ = 1; *option++ = 3; *option++ = 6; *option++ = 15;
    *option++ = 61; *option++ = 7; *option++ = 1; memcpy(option, program.local, 6); option += 6;
    *option++ = 255;
    program.probe_sends++;
    Emit("event=probe_dhcp_discover_sent attempt=%u", program.probe_sends);
    QueueToPhone(frame, sizeof frame);
}

static void InspectProbeReply(const uint8_t *frame, uint16_t size) {
    if (size < 14 + 20 + 8 + 240) return;
    if (((frame[12] << 8) | frame[13]) != ETH_IPV4) return;
    const uint8_t *ip = frame + 14;
    size_t header_len = (size_t)(ip[0] & 0x0f) * 4;
    if (ip[9] != 17 || 14 + header_len + 8 + 240 > size) return;
    const uint8_t *udp = ip + header_len;
    if (((udp[0] << 8) | udp[1]) != 67) return;
    const uint8_t *bootp = udp + 8;
    uint32_t xid;
    memcpy(&xid, bootp + 4, 4);
    if (bootp[0] != 2 || ntohl(xid) != program.xid) return;
    char offered[32] = "", router[32] = "", dns[32] = "", mask[32] = "", server[32] = "";
    inet_ntop(AF_INET, bootp + 16, offered, sizeof offered);
    const uint8_t *option = bootp + 240;
    const uint8_t *end = frame + size;
    unsigned lease = 0;
    while (option + 2 <= end && *option != 255) {
        if (*option == 0) { option++; continue; }
        uint8_t tag = option[0], length = option[1];
        const uint8_t *value = option + 2;
        if (value + length > end) break;
        if (tag == 3 && length >= 4) inet_ntop(AF_INET, value, router, sizeof router);
        if (tag == 6 && length >= 4) inet_ntop(AF_INET, value, dns, sizeof dns);
        if (tag == 1 && length == 4) inet_ntop(AF_INET, value, mask, sizeof mask);
        if (tag == 54 && length == 4) inet_ntop(AF_INET, value, server, sizeof server);
        if (tag == 51 && length == 4) lease = (unsigned)((value[0] << 24) | (value[1] << 16) | (value[2] << 8) | value[3]);
        option += 2 + length;
    }
    Emit("event=probe_dhcp_offer ip=%s mask=%s router=%s dns=%s server=%s lease=%us", offered, mask, router, dns, server, lease);
    Emit("event=probe_result ok=true");
    program.probe_done = true;
    Stop(0);
}

// ---------------------------------------------------------------- connection management

static void Connect(void) {
    if (program.stopping || program.bnep_open || program.connecting) return;
    program.connecting = true;
    program.connect_attempts++;
    Emit("event=bnep_connecting attempt=%u phone=%s", program.connect_attempts, program.phone_str);
    int status = bnep_connect(packet_handler, program.phone, BNEP_PSM,
                              BLUETOOTH_SERVICE_CLASS_PANU, BLUETOOTH_SERVICE_CLASS_NAP);
    if (status != 0) {
        program.connecting = false;
        Emit("event=bnep_connect_call_failed status=%d", status);
    }
}

static void ReconnectTimer(btstack_timer_source_t *timer) {
    (void)timer;
    Connect();
}

static void ScheduleReconnect(uint32_t delay_ms) {
    if (program.stopping) return;
    btstack_run_loop_remove_timer(&program.reconnect_timer);
    btstack_run_loop_set_timer_handler(&program.reconnect_timer, ReconnectTimer);
    btstack_run_loop_set_timer(&program.reconnect_timer, delay_ms);
    btstack_run_loop_add_timer(&program.reconnect_timer);
}

static void Tick(btstack_timer_source_t *timer) {
    uint64_t now = NowMs();
    if (stop_requested && !program.stopping) Stop(0);
    if (program.stopping && now >= program.stop_deadline_ms) FinishAndExit(program.exit_status);
    if (program.probe_mode && program.bnep_open && !program.probe_done) {
        static uint64_t last_probe_ms;
        if (now - last_probe_ms >= 2000) {
            last_probe_ms = now;
            if (program.probe_sends >= 6) {
                Emit("event=probe_result ok=false reason=no_dhcp_offer");
                Stop(5);
            } else {
                SendDiscover();
            }
        }
    }
    if (program.bnep_open && now - program.last_stats_ms >= 15000) {
        program.last_stats_ms = now;
        Emit("event=stats to_phone=%llu/%lluB from_phone=%llu/%lluB dropped=%llu queue=%u",
             (unsigned long long)program.to_phone_frames, (unsigned long long)program.to_phone_bytes,
             (unsigned long long)program.from_phone_frames, (unsigned long long)program.from_phone_bytes,
             (unsigned long long)program.dropped, program.txq_count);
    }
    btstack_run_loop_set_timer(timer, 200);
    btstack_run_loop_add_timer(timer);
}

static void ReadStdin(btstack_data_source_t *source, btstack_data_source_callback_type_t type) {
    (void)type;
    char line[64];
    ssize_t got = read(STDIN_FILENO, line, sizeof line - 1);
    if (got == 0) {
        btstack_run_loop_disable_data_source_callbacks(source, DATA_SOURCE_CALLBACK_READ);
        return;
    }
    if (got < 0) return;
    line[got] = 0;
    if (strncmp(line, "quit", 4) == 0) Stop(0);
}

// ---------------------------------------------------------------- Bluetooth events

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    (void)channel;
    bd_addr_t address;
    if (packet_type == BNEP_DATA_PACKET) {
        if (program.probe_mode) {
            program.from_phone_frames++;
            InspectProbeReply(packet, size);
        } else {
            DeliverFromPhone(packet, size);
        }
        return;
    }
    if (packet_type != HCI_EVENT_PACKET) return;

    switch (hci_event_packet_get_type(packet)) {
        case BTSTACK_EVENT_STATE: {
            uint8_t state = btstack_event_state_get_state(packet);
            if (state == HCI_STATE_WORKING) {
                gap_local_bd_addr(address);
                Emit("event=stack_ready local=%s", bd_addr_to_str(address));
                if (memcmp(address, program.local, 6) != 0) {
                    Emit("event=local_address_mismatch wanted=%s", program.local_str);
                    Stop(6);
                    break;
                }
                Connect();
            } else if (state == HCI_STATE_OFF && program.stopping) {
                FinishAndExit(program.exit_status);
            }
            break;
        }
        case HCI_EVENT_CONNECTION_COMPLETE:
            Emit("event=acl_connection status=0x%02x", hci_event_connection_complete_get_status(packet));
            break;
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            Emit("event=acl_disconnected reason=0x%02x", hci_event_disconnection_complete_get_reason(packet));
            break;
        case HCI_EVENT_PIN_CODE_REQUEST:
            hci_event_pin_code_request_get_bd_addr(packet, address);
            Emit("event=legacy_pin_request_rejected");
            gap_pin_code_negative(address);
            break;
        case HCI_EVENT_USER_CONFIRMATION_REQUEST:
            hci_event_user_confirmation_request_get_bd_addr(packet, address);
            if (memcmp(address, program.phone, 6) == 0) {
                Emit("event=pairing_confirm code=%06u note=tap_Pair_on_the_phone",
                     (unsigned)hci_event_user_confirmation_request_get_numeric_value(packet));
                gap_ssp_confirmation_response(address);
            } else {
                Emit("event=pairing_from_unknown_device_rejected");
                gap_ssp_confirmation_negative(address);
            }
            break;
        case HCI_EVENT_SIMPLE_PAIRING_COMPLETE:
            Emit("event=pairing_complete status=0x%02x", hci_event_simple_pairing_complete_get_status(packet));
            break;
        case HCI_EVENT_AUTHENTICATION_COMPLETE: {
            uint8_t status = hci_event_authentication_complete_get_status(packet);
            Emit("event=authentication status=0x%02x", status);
            if (status == ERROR_CODE_PIN_OR_KEY_MISSING || status == ERROR_CODE_AUTHENTICATION_FAILURE) {
                Emit("event=dropping_stale_key");
                gap_drop_link_key_for_bd_addr(program.phone);
            }
            break;
        }
        case HCI_EVENT_ENCRYPTION_CHANGE:
            Emit("event=encryption status=0x%02x enabled=%u",
                 hci_event_encryption_change_get_status(packet),
                 hci_event_encryption_change_get_encryption_enabled(packet));
            break;
        case HCI_EVENT_LINK_KEY_NOTIFICATION:
            Emit("event=link_key_stored");
            break;
        case HCI_EVENT_ROLE_CHANGE:
            Emit("event=role status=0x%02x role=%s", packet[2], packet[9] == 0 ? "central" : "peripheral");
            break;
        case HCI_EVENT_MODE_CHANGE:
            Emit("event=link_mode status=0x%02x mode=%u interval_slots=%u", packet[2], packet[5],
                 little_endian_read_16(packet, 6));
            break;
        case HCI_EVENT_CONNECTION_PACKET_TYPE_CHANGED:
            Emit("event=packet_types status=0x%02x types=0x%04x", packet[2], little_endian_read_16(packet, 5));
            break;
        case HCI_EVENT_MAX_SLOTS_CHANGED:
            Emit("event=max_slots slots=%u", packet[4]);
            break;
        case BNEP_EVENT_CHANNEL_OPENED: {
            program.connecting = false;
            uint8_t status = bnep_event_channel_opened_get_status(packet);
            if (status != 0) {
                Emit("event=bnep_open_failed status=0x%02x", status);
                ScheduleReconnect(program.connect_attempts < 3 ? 2000 : 6000);
                break;
            }
            program.bnep_cid = bnep_event_channel_opened_get_bnep_cid(packet);
            program.bnep_open = true;
            program.txq_head = program.txq_count = 0;
            program.last_stats_ms = NowMs();
            Emit("event=bnep_open cid=%u mtu=%u source_uuid=0x%04x dest_uuid=0x%04x",
                 program.bnep_cid, bnep_event_channel_opened_get_mtu(packet),
                 bnep_event_channel_opened_get_source_uuid(packet),
                 bnep_event_channel_opened_get_destination_uuid(packet));
            break;
        }
        case BNEP_EVENT_CHANNEL_TIMEOUT:
            Emit("event=bnep_timeout");
            break;
        case BNEP_EVENT_CHANNEL_CLOSED:
            Emit("event=bnep_closed");
            program.bnep_open = false;
            program.connecting = false;
            program.txq_head = program.txq_count = 0;
            if (program.probe_mode && !program.probe_done && !program.stopping) {
                Emit("event=probe_result ok=false reason=channel_closed");
                Stop(5);
            } else {
                ScheduleReconnect(3000);
            }
            break;
        case BNEP_EVENT_CAN_SEND_NOW:
            SendOneToPhone();
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------- main

static const char *ArgValue(int argc, char **argv, const char *name) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (strcmp(argv[index], name) == 0) return argv[index + 1];
    }
    return NULL;
}

int main(int argc, char **argv) {
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);
    signal(SIGPIPE, SIG_IGN);

    const char *phone = ArgValue(argc, argv, "--phone");
    const char *local = ArgValue(argc, argv, "--local-address");
    const char *state_db = ArgValue(argc, argv, "--state-db");
    program.netif = ArgValue(argc, argv, "--netif");
    program.probe_mode = program.netif == NULL;
    program.bpf_fd = -1;
    program.xid = arc4random();

    if (phone == NULL || local == NULL || state_db == NULL ||
        !sscanf_bd_addr(phone, program.phone) || !sscanf_bd_addr(local, program.local)) {
        fprintf(stderr, "usage: %s --phone XX:XX:XX:XX:XX:XX --local-address XX:XX:XX:XX:XX:XX "
                        "--state-db FILE [--netif feth1]\n", argv[0]);
        return 2;
    }
    if ((program.local[0] & 0x02) == 0) {
        fprintf(stderr, "The local address must be a locally administered address.\n");
        return 2;
    }
    strlcpy(program.phone_str, bd_addr_to_str(program.phone), sizeof program.phone_str);
    strlcpy(program.local_str, bd_addr_to_str(program.local), sizeof program.local_str);
    Emit("event=start mode=%s phone=%s local=%s", program.probe_mode ? "probe" : "bridge",
         program.phone_str, program.local_str);

    if (!program.probe_mode) {
        program.bpf_fd = OpenBpf(program.netif);
        if (program.bpf_fd < 0) return 3;
    }

    btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_posix_get_instance());
    if (getenv("PANHOST_HCI_LOG") != NULL) {
        hci_dump_init(hci_dump_posix_stdout_get_instance());
    }

    hci_init(hci_transport_skywalk_instance(SkywalkHCINativeDriver()), NULL);
    hci_transport_skywalk_set_fault_handler(TransportFailed, &program);
    hci_set_chipset(BroadcomAddressTestChipset());
    hci_set_bd_addr(program.local);

    const btstack_tlv_t *tlv = btstack_tlv_posix_init_instance(&program.tlv_context, state_db);
    btstack_tlv_set_instance(tlv, &program.tlv_context);
    hci_set_link_key_db(btstack_link_key_db_tlv_get_instance(tlv, &program.tlv_context));

    l2cap_init();
    sdp_init();
    bnep_init();
    bnep_register_service(packet_handler, BLUETOOTH_SERVICE_CLASS_PANU, 1691);
    static uint8_t panu_sdp_record[220];
    uint16_t network_packet_types[] = {ETH_IPV4, ETH_ARP, ETH_IPV6, 0};
    pan_create_panu_sdp_record(panu_sdp_record, sdp_create_service_record_handle(),
                               network_packet_types, NULL, NULL, BNEP_SECURITY_NONE);
    sdp_register_service(panu_sdp_record);

    static btstack_packet_callback_registration_t hci_registration;
    hci_registration.callback = packet_handler;
    hci_add_event_handler(&hci_registration);

    gap_set_local_name("Mac Tether");
    gap_set_class_of_device(0x02010C);
    gap_discoverable_control(0);
    gap_connectable_control(1);
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH);
    gap_set_allow_role_switch(true);
    gap_set_bondable_mode(1);
    gap_ssp_set_io_capability(SSP_IO_CAPABILITY_DISPLAY_YES_NO);

    if (program.bpf_fd >= 0) {
        btstack_run_loop_set_data_source_fd(&program.bpf_source, program.bpf_fd);
        btstack_run_loop_set_data_source_handler(&program.bpf_source, ReadBpf);
        btstack_run_loop_add_data_source(&program.bpf_source);
        btstack_run_loop_enable_data_source_callbacks(&program.bpf_source, DATA_SOURCE_CALLBACK_READ);
    }

    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags >= 0) fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    btstack_run_loop_set_data_source_fd(&program.stdin_source, STDIN_FILENO);
    btstack_run_loop_set_data_source_handler(&program.stdin_source, ReadStdin);
    btstack_run_loop_add_data_source(&program.stdin_source);
    btstack_run_loop_enable_data_source_callbacks(&program.stdin_source, DATA_SOURCE_CALLBACK_READ);

    btstack_run_loop_set_timer_handler(&program.tick_timer, Tick);
    btstack_run_loop_set_timer(&program.tick_timer, 200);
    btstack_run_loop_add_timer(&program.tick_timer);

    int hci_status = hci_power_control(HCI_POWER_ON);
    if (hci_status != 0) {
        Emit("event=stack_start_failed status=%d", hci_status);
        return 4;
    }
    btstack_run_loop_execute();
    return 0;
}
