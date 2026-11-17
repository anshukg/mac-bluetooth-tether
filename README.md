# Bluetooth tethering for Apple silicon Macs

macOS has had no Bluetooth tethering since macOS 12. This brings it back for Android phones: pair your phone with your Mac once, install this, and when your Mac has no other network it goes online through the phone over Bluetooth by itself. When Wi-Fi comes back, it switches back and your Bluetooth headphones and mouse return, with their sound restored.

You never have to type a command after installing.

## Requirements

- An Apple silicon Mac whose Bluetooth chip is a Broadcom chip on PCIe. Run `./install.sh --check` to find out; it changes nothing.
- An Android phone with Bluetooth tethering, already paired with the Mac in System Settings > Bluetooth.
- Apple's command line tools: `xcode-select --install`.

## Install

```sh
git clone https://github.com/anshukg/mac-bluetooth-tether.git
cd mac-bluetooth-tether
./install.sh
```

During the install:

1. It checks your Mac and phone and builds the tools. The first build downloads [BTstack](https://github.com/bluekitchen/btstack).
2. It runs a one-minute test with your phone. Switch on **Bluetooth tethering** on the phone first. On Samsung phones it is under Settings > Connections > Mobile Hotspot and Tethering. Your other Bluetooth devices pause during the test.
3. The phone asks once to pair with **Mac Tether**. Tap **Pair**.
4. macOS asks for your password once, to install the network helper.
5. macOS asks once whether **Bluetooth Tether** may use Bluetooth. Choose **Allow**.

If anything fails, the installer stops with the reason and leaves your Mac as it was. You can run it again safely.

To find out whether your Mac and phone work together before installing anything permanent, run `./install.sh --try`. It runs the test only.

## Everyday use

There is nothing to do. When the Mac loses its network and the phone is nearby, tethering starts within about 20 seconds and a hotspot icon appears in the menu bar. When Wi-Fi is back, tethering stops, the icon goes away, and your Bluetooth devices reconnect.

Commands, if you want them:

| Command | What it does |
|---|---|
| `bt-tether status` | Shows what is happening now |
| `bt-tether only` | Turns tethering on, checks it, then switches Wi-Fi off |
| `bt-tether wifi` | Switches Wi-Fi back on, then turns tethering off |
| `bt-tether start` / `stop` | Turns tethering on or off by hand |
| `bt-tether auto off` / `on` | Pauses or resumes automatic switching |
| `bt-tether doctor` | Checks the Mac and the phone |
| `bt-tether repair-pairing` | Redoes the Mac's own pairing with the phone |

## What to expect

- **One radio, one job.** While tethering is on, the tool runs the Mac's Bluetooth chip itself. Headphones, mice and other Bluetooth devices pause, and the Bluetooth switch shows off. They return by themselves when tethering ends. Headphones on a cable keep working.
- **Bluetooth speed.** The measured speed is about 1 Mbps down and 1.3 Mbps up. That is the limit of the Bluetooth radio, not of your phone's connection. Bluetooth tethering is for staying connected, not for large downloads.
- **Switching Bluetooth on does not end tethering.** If you switch Bluetooth on during tethering, the tool takes the radio back within a few seconds and shows a notification.
- **Lost phone.** If the phone stops answering for 45 seconds, tethering ends and your Bluetooth devices come back. It resumes by itself when the phone is nearby again.

## What it installs

| Where | What |
|---|---|
| `~/Library/Application Support/bt-tether/` | The tools, settings, logs, and the pairing key for "Mac Tether" |
| `~/Library/LaunchAgents/com.anshu.bt-tether.auto.plist` | A login item that runs the automatic switching |
| `/Library/LaunchDaemons/com.anshu.bt-tether.net.plist` | Creates a virtual network port at startup |
| `/Library/LaunchDaemons/com.anshu.bt-tether.link.plist` | Starts or clears that port's address when tethering starts or stops |
| `/Library/Application Support/bt-tether/` | A root-owned copy of the script those two jobs run |
| `/usr/local/bin/bt-tether` | The command |

At every startup, the network helper also gives your user account read and write access to the Mac's packet-capture devices (`/dev/bpf*`). The tool needs this to pass network traffic between the virtual port and the phone, and Wireshark's installer makes the same change. It means any program you run can capture network traffic. Uninstalling returns those devices to root.

No passwordless administrator rule is created, and nothing is changed in macOS itself.

## Uninstall

```sh
./uninstall.sh
```

Uninstalling removes everything in the table above and asks for your password once. Afterwards, also remove "Mac Tether" from your phone's Bluetooth devices. To keep the pairing for a later reinstall, run `./uninstall.sh --keep-pairing`.

## Troubleshooting

| What you see | What to do |
|---|---|
| "the phone refused tethering" | Switch on Bluetooth tethering on the phone. |
| "did not answer over Bluetooth" | Keep the phone nearby and unlocked with Bluetooth on. If it keeps failing, run `bt-tether repair-pairing`. |
| The phone rejected "Mac Tether" | On the phone, remove "Mac Tether" from Bluetooth devices, then run `./install.sh` again. |
| Bluetooth stays off after something went wrong | Run `bt-tether stop`. A restart always fixes it. |
| Anything else | Run `bt-tether doctor`. The logs are in `~/Library/Application Support/bt-tether/`. |

## How it works

macOS refuses the Bluetooth channel that tethering needs: Apple's Bluetooth service caps classic channels at 1024 bytes, and Android requires 1691. So, while tethering is on, this tool pauses Apple's Bluetooth service and runs [BTstack](https://github.com/bluekitchen/btstack) on the Mac's Bluetooth chip directly. It reaches the chip through the same shared-memory channels Apple's service uses.

The tether uses its own Bluetooth address, so its pairing never interferes with the Mac's normal one. Network traffic passes through a virtual Ethernet pair (`feth`), so macOS treats the phone like any other network. When tethering ends, the chip gets its own address back and Apple's Bluetooth service resumes.

## Tested

Verified on a MacBook Air with M4 (Bluetooth chip BCM_4388C2), macOS 26.5.2, with a Samsung Galaxy Z Fold7. Other Apple silicon Macs with Broadcom PCIe Bluetooth are expected to work but are untested. Run `./install.sh --try` to find out on yours. Intel Macs are not supported. iPhones are untested.

Not yet observed: automatic start after a real restart, and behaviour across sleep and wake.

## Licence

The files in this repository are under the MIT licence; see [LICENSE](LICENSE).

The build downloads BTstack, whose licence allows use only for personal, non-commercial purposes. The BTstack patch in `third_party/` falls under that licence too. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
