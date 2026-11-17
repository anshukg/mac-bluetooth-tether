// tap.m — check or change the Mac's own Bluetooth link to a paired device, through Apple's
// Bluetooth stack. Other Bluetooth devices are not disturbed.
//
//   tap AA-BB-CC-DD-EE-FF            connect (exit 0 when connected)
//   tap AA-BB-CC-DD-EE-FF --close    disconnect
//   tap AA-BB-CC-DD-EE-FF --status   print paired/connected (exit 0 when connected)
//   tap AA-BB-CC-DD-EE-FF --nap      connect, then report whether the device offers
//                                    Bluetooth tethering (prints nap=1 or nap=0)
//
// Build: clang -fobjc-arc -framework Foundation -framework IOBluetooth tap.m -o tap

#import <Foundation/Foundation.h>
#import <IOBluetooth/IOBluetooth.h>

@interface SDPWaiter : NSObject
@property BOOL done;
@end

@implementation SDPWaiter
- (void)sdpQueryComplete:(IOBluetoothDevice *)device status:(IOReturn)status {
    self.done = YES;
}
@end

static int Connect(IOBluetoothDevice *device) {
    if (device.isConnected) {
        printf("result=already_connected\n");
        return 0;
    }
    CFAbsoluteTime started = CFAbsoluteTimeGetCurrent();
    IOReturn result = [device openConnection];
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1.0]];
    printf("result=connect status=0x%x connected_after=%d seconds=%.1f\n",
           result, device.isConnected, CFAbsoluteTimeGetCurrent() - started);
    return (result == kIOReturnSuccess && device.isConnected) ? 0 : 1;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 2) {
            fprintf(stderr, "usage: tap <bt-address> [--close|--status|--nap]\n");
            return 2;
        }
        IOBluetoothDevice *device =
            [IOBluetoothDevice deviceWithAddressString:[NSString stringWithUTF8String:argv[1]]];
        if (!device) {
            printf("result=no_such_device\n");
            return 2;
        }
        const char *option = argc > 2 ? argv[2] : "";
        printf("device=%s paired=%d connected_before=%d\n",
               device.name.UTF8String ?: "?", device.isPaired, device.isConnected);

        if (strcmp(option, "--status") == 0) return device.isConnected ? 0 : 1;

        if (strcmp(option, "--close") == 0) {
            IOReturn result = [device closeConnection];
            printf("result=close status=0x%x\n", result);
            return result == kIOReturnSuccess ? 0 : 1;
        }

        if (strcmp(option, "--nap") == 0) {
            if (Connect(device) != 0) return 3;
            SDPWaiter *waiter = [SDPWaiter new];
            if ([device performSDPQuery:waiter] == kIOReturnSuccess) {
                NSDate *until = [NSDate dateWithTimeIntervalSinceNow:10];
                while (!waiter.done && [until timeIntervalSinceNow] > 0) {
                    [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                             beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.2]];
                }
            }
            IOBluetoothSDPServiceRecord *nap =
                [device getServiceRecordForUUID:[IOBluetoothSDPUUID uuid16:0x1116]];
            printf("nap=%d\n", nap != nil);
            return nap != nil ? 0 : 4;
        }

        return Connect(device);
    }
}
