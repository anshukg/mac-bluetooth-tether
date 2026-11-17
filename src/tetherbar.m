// tetherbar.m — menu-bar indicator shown while Bluetooth tethering is on.
// Build: clang -fobjc-arc -framework Cocoa tetherbar.m -o build/tetherbar
// Run:   build/tetherbar /path/to/bt-tether      (started and stopped by bt-tether itself)

#import <Cocoa/Cocoa.h>

@interface BarDelegate : NSObject <NSApplicationDelegate>
@property NSStatusItem *item;
@property NSString *tool;
@end

@implementation BarDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)note {
    self.item = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
    NSImage *icon = [NSImage imageWithSystemSymbolName:@"personalhotspot"
                              accessibilityDescription:@"Bluetooth tethering"];
    if (icon) {
        self.item.button.image = icon;
    } else {
        self.item.button.title = @"Tether";
    }
    self.item.button.toolTip = @"Internet through your phone, over Bluetooth";

    NSMenu *menu = [NSMenu new];
    [menu addItemWithTitle:@"Internet: through your phone, over Bluetooth" action:nil keyEquivalent:@""];
    [menu addItemWithTitle:@"The Bluetooth switch shows off while tethering uses the radio" action:nil keyEquivalent:@""];
    [menu addItemWithTitle:@"Headphones and other Bluetooth devices return when tethering ends" action:nil keyEquivalent:@""];
    [menu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *back = [menu addItemWithTitle:@"Switch Back to Wi-Fi" action:@selector(backToWiFi:) keyEquivalent:@""];
    back.target = self;
    self.item.menu = menu;
}

- (void)backToWiFi:(id)sender {
    NSTask *task = [NSTask new];
    task.executableURL = [NSURL fileURLWithPath:self.tool];
    task.arguments = @[@"wifi"];
    [task launchAndReturnError:nil];
}
@end

int main(int argc, const char **argv) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        BarDelegate *delegate = [BarDelegate new];
        delegate.tool = argc > 1 ? [NSString stringWithUTF8String:argv[1]] : @"/usr/local/bin/bt-tether";
        app.delegate = delegate;
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [app run];
    }
    return 0;
}
