#ifndef ANDROID_MAC_CALL_BRIDGE_CALL_HOST_BTSTACK_CONFIG_H
#define ANDROID_MAC_CALL_BRIDGE_CALL_HOST_BTSTACK_CONFIG_H

#define HAVE_ASSERT
#define HAVE_MALLOC
#define HAVE_POSIX_FILE_IO
#define HAVE_POSIX_TIME

#define ENABLE_CLASSIC
#define ENABLE_HFP_WIDE_BAND_SPEECH
#define ENABLE_LOG_ERROR
#define ENABLE_LOG_INFO
#define ENABLE_PRINTF_HEXDUMP
#define ENABLE_SCO_OVER_HCI
/* Apple's PCIe controller accepts the command that enables controller-to-host
 * SCO completion reports, but does not emit those reports on this transport.
 * Pace uplink from each received SCO packet instead. */
#define DISABLE_HCI_SYNCHRONOUS_FLOW_CONTROL
#define HCI_ACL_PAYLOAD_SIZE (1691 + 4)
#define HCI_INCOMING_PRE_BUFFER_SIZE 14
#define NVM_NUM_LINK_KEYS 16

#endif
