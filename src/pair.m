// pair.m — drop a stale pairing record and re-pair (classic Bluetooth) from the command line.
// Build: clang -fobjc-arc -framework Foundation -framework IOBluetooth pair.m -o pair
// Run:   ./pair AA-BB-CC-DD-EE-FF [--keep]     (--keep = do not remove the old record first)

#import <Foundation/Foundation.h>
#import <IOBluetooth/IOBluetooth.h>

static void L(NSString *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    NSString *s = [[NSString alloc] initWithFormat:fmt arguments:ap];
    va_end(ap);
    NSDateFormatter *f = [NSDateFormatter new]; f.dateFormat = @"HH:mm:ss.SSS";
    fprintf(stderr, "[%s] %s\n", [f stringFromDate:[NSDate date]].UTF8String, s.UTF8String);
}

@interface PairDelegate : NSObject <IOBluetoothDevicePairDelegate>
@property BOOL finished;
@property IOReturn result;
@end

@implementation PairDelegate
- (void)devicePairingStarted:(id)sender { L(@"pairing started"); }
- (void)devicePairingConnecting:(id)sender { L(@"connecting to phone..."); }
- (void)devicePairingConnected:(id)sender { L(@"connected, negotiating"); }
- (void)devicePairingPINCodeRequest:(id)sender {
    L(@"phone asked for a PIN; replying 0000");
    BluetoothPINCode pin; memset(&pin, 0, sizeof pin); memcpy(pin.data, "0000", 4);
    [sender replyPINCode:4 PINCode:&pin];
}
- (void)devicePairingUserConfirmationRequest:(id)sender numericValue:(BluetoothNumericValue)numericValue {
    L(@"CONFIRM CODE: %06u  (accepting on the Mac side; tap Pair on the phone)", numericValue);
    [sender replyUserConfirmation:YES];
}
- (void)devicePairingUserPasskeyNotification:(id)sender passkey:(BluetoothPasskey)passkey {
    L(@"PASSKEY to type on the phone: %06u", passkey);
}
- (void)deviceSimplePairingComplete:(id)sender status:(BluetoothHCIEventStatus)status {
    L(@"simple pairing complete, HCI status 0x%02x", status);
}
- (void)devicePairingFinished:(id)sender error:(IOReturn)error {
    L(@"pairing finished, result 0x%x", error);
    self.result = error; self.finished = YES;
}
@end

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 2) { fprintf(stderr, "usage: pair <bt-address> [--keep]\n"); return 2; }
        NSString *addr = [NSString stringWithUTF8String:argv[1]];
        BOOL keep = argc > 2 && strcmp(argv[2], "--keep") == 0;
        IOBluetoothDevice *dev = [IOBluetoothDevice deviceWithAddressString:addr];
        if (!dev) { L(@"no such device"); return 2; }
        L(@"device %@ paired=%d connected=%d", dev.name, dev.isPaired, dev.isConnected);

        if (!keep && dev.isPaired) {
            SEL rm = NSSelectorFromString(@"remove");
            if ([dev respondsToSelector:rm]) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
                [dev performSelector:rm];
#pragma clang diagnostic pop
                L(@"old pairing record removed on the Mac");
            } else {
                L(@"cannot remove record programmatically");
            }
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:2.0]];
            dev = [IOBluetoothDevice deviceWithAddressString:addr];
            L(@"after removal: paired=%d", dev.isPaired);
        }

        PairDelegate *d = [PairDelegate new];
        IOBluetoothDevicePair *p = [IOBluetoothDevicePair pairWithDevice:dev];
        p.delegate = d;
        IOReturn r = [p start];
        L(@"pair start -> 0x%x", r);
        if (r != kIOReturnSuccess) return 1;

        NSDate *until = [NSDate dateWithTimeIntervalSinceNow:80];
        while (!d.finished && [until timeIntervalSinceNow] > 0)
            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.2]];

        dev = [IOBluetoothDevice deviceWithAddressString:addr];
        L(@"RESULT: finished=%d result=0x%x paired=%d", d.finished, d.result, dev.isPaired);
        return (d.finished && d.result == kIOReturnSuccess) ? 0 : 1;
    }
}
