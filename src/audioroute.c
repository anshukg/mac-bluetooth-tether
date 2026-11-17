// audioroute.c — read and restore the Mac's sound output device.
// Build: clang -framework CoreAudio -framework CoreFoundation audioroute.c -o build/audioroute
//
//   audioroute get                    prints "<name>\t<bluetooth|other>" for the default output
//   audioroute list                   prints every output device name
//   audioroute set <name>             makes <name> the default and system output
//   audioroute wait-set <name> <sec>  waits up to <sec> for <name> to appear, then sets it

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static AudioObjectPropertyAddress Address(AudioObjectPropertySelector selector, AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress address = {selector, scope, kAudioObjectPropertyElementMain};
    return address;
}

static int DeviceName(AudioDeviceID device, char *buffer, size_t capacity) {
    CFStringRef name = NULL;
    UInt32 size = sizeof name;
    AudioObjectPropertyAddress address = Address(kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal);
    if (AudioObjectGetPropertyData(device, &address, 0, NULL, &size, &name) != noErr || name == NULL) return -1;
    Boolean ok = CFStringGetCString(name, buffer, (CFIndex)capacity, kCFStringEncodingUTF8);
    CFRelease(name);
    return ok ? 0 : -1;
}

static int HasOutput(AudioDeviceID device) {
    UInt32 size = 0;
    AudioObjectPropertyAddress address = Address(kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput);
    if (AudioObjectGetPropertyDataSize(device, &address, 0, NULL, &size) != noErr) return 0;
    return size > 0;
}

static int IsBluetooth(AudioDeviceID device) {
    UInt32 transport = 0, size = sizeof transport;
    AudioObjectPropertyAddress address = Address(kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal);
    if (AudioObjectGetPropertyData(device, &address, 0, NULL, &size, &transport) != noErr) return 0;
    return transport == kAudioDeviceTransportTypeBluetooth || transport == kAudioDeviceTransportTypeBluetoothLE;
}

static AudioDeviceID DefaultOutput(void) {
    AudioDeviceID device = kAudioObjectUnknown;
    UInt32 size = sizeof device;
    AudioObjectPropertyAddress address = Address(kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, &device);
    return device;
}

static AudioDeviceID FindOutput(const char *wanted, int print_all) {
    UInt32 size = 0;
    AudioObjectPropertyAddress address = Address(kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal);
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, NULL, &size) != noErr) return kAudioObjectUnknown;
    AudioDeviceID *devices = malloc(size);
    if (devices == NULL) return kAudioObjectUnknown;
    AudioDeviceID found = kAudioObjectUnknown;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, NULL, &size, devices) == noErr) {
        for (UInt32 index = 0; index < size / sizeof(AudioDeviceID); ++index) {
            char name[256];
            if (!HasOutput(devices[index]) || DeviceName(devices[index], name, sizeof name) != 0) continue;
            if (print_all) printf("%s\n", name);
            if (wanted != NULL && strcmp(name, wanted) == 0) { found = devices[index]; break; }
        }
    }
    free(devices);
    return found;
}

static int SetOutput(AudioDeviceID device) {
    AudioObjectPropertyAddress output = Address(kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal);
    AudioObjectPropertyAddress system_output = Address(kAudioHardwarePropertyDefaultSystemOutputDevice, kAudioObjectPropertyScopeGlobal);
    OSStatus first = AudioObjectSetPropertyData(kAudioObjectSystemObject, &output, 0, NULL, sizeof device, &device);
    AudioObjectSetPropertyData(kAudioObjectSystemObject, &system_output, 0, NULL, sizeof device, &device);
    return first == noErr ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "get") == 0) {
        AudioDeviceID device = DefaultOutput();
        char name[256];
        if (device == kAudioObjectUnknown || DeviceName(device, name, sizeof name) != 0) return 1;
        printf("%s\t%s\n", name, IsBluetooth(device) ? "bluetooth" : "other");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "list") == 0) {
        FindOutput(NULL, 1);
        return 0;
    }
    if (argc >= 3 && (strcmp(argv[1], "set") == 0 || strcmp(argv[1], "wait-set") == 0)) {
        int tries = 1;
        if (strcmp(argv[1], "wait-set") == 0) tries = (argc >= 4 ? atoi(argv[3]) : 15) * 2;
        for (int attempt = 0; attempt < tries; ++attempt) {
            AudioDeviceID device = FindOutput(argv[2], 0);
            if (device != kAudioObjectUnknown) {
                if (DefaultOutput() == device) return 0;
                return SetOutput(device);
            }
            usleep(500000);
        }
        return 1;
    }
    fprintf(stderr, "usage: audioroute get | list | set <name> | wait-set <name> <seconds>\n");
    return 2;
}
