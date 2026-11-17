# Bluetooth Tether for Mac

**Use your Android phone's internet on your Mac, over Bluetooth, automatically.**

When your Mac loses Wi-Fi, it switches to your phone's internet by itself. When Wi-Fi comes back, it switches back by itself. You set it up once, about 5 minutes, and then you never touch it again.

---

## Will it work for me?

Check these three things first:

1. **Your Mac has Apple silicon.** Click the Apple menu  at the top left of the screen, then **About This Mac**. Next to **Chip** it should say **Apple M1**, **M2**, **M3**, **M4** or later. If you see **Processor** and **Intel**, this will not work on your Mac.
2. **Your phone is an Android phone.** Samsung, Google Pixel, Motorola, OnePlus, Xiaomi and so on. iPhones have not been tried.
3. **You know your Mac password.** It is the password you type to log in to your Mac.

Not sure about something? The setup checks everything first and tells you, in plain words, if something will not work. It does not change anything until those checks pass.

---

## Set it up (about 5 minutes)

### Step 1. Open Terminal

Terminal is an app that comes with every Mac. You only need it for setup.

- Press **Command (⌘) and Space** together. A search box appears.
- Type **Terminal** and press **Return**.

A window with some text in it opens. That is Terminal.

### Step 2. Copy this line, paste it into Terminal, press Return

```
/bin/zsh -c "$(curl -fsSL https://raw.githubusercontent.com/anshukg/mac-bluetooth-tether/main/get.sh)"
```

- Copy the line. Hover over the grey box above and click the copy button at its right edge, or select the whole line and press **Command (⌘) and C**.
- Click inside the Terminal window.
- Press **Command (⌘) and V** to paste, then press **Return**.

### Step 3. Follow what Terminal says

The setup talks you through everything. It explains each step before it happens, and it waits whenever it needs you. When it says **Press Return**, press the Return key.

Along the way, these boxes appear. This is what to do with each:

| What appears | Where | What to do |
|---|---|---|
| "Install command line developer tools?" (only on some Macs) | Mac | Click **Install**, then **Agree**. Wait 5 to 15 minutes. The setup continues by itself. |
| "Terminal would like to use Bluetooth" | Mac | Click **Allow**. |
| "Pair with Mac Tether?" with a number | Phone | Tap **Pair**. |
| A question about contacts or call history | Phone | Either answer is fine. |
| "Bluetooth Tether needs your Mac password…" | Mac | Type your Mac password and click **OK**. |
| "Bluetooth Tether would like to use Bluetooth" | Mac | Click **Allow**. |

When it is finished, it says **All set.** and offers to show you it working. Say yes: it asks you to switch Wi-Fi off, shows your Mac going online through your phone, and then has you switch Wi-Fi back on.

You can close the Terminal window afterwards.

**To stop the setup at any time**, press **Control and C** together. Nothing breaks: run the same line again whenever you are ready.

---

## Switch on "Bluetooth tethering" on your phone

The setup reminds you when to do this. Bluetooth tethering is a switch on your phone that lets other devices use its internet over Bluetooth.

| Phone | Where the switch is |
|---|---|
| Samsung | Settings > Connections > Mobile Hotspot and Tethering > **Bluetooth tethering** |
| Google Pixel, Motorola and most others | Settings > Network & internet > Hotspot & tethering > **Bluetooth tethering** |
| Any other phone | Open Settings, tap the search box at the top, type **Bluetooth tethering**, and switch it on. |

Leave it switched on. It does nothing until your Mac needs it.

### If your phone is not paired with your Mac yet

The setup notices and walks you through it. In short:

1. On the phone, open **Settings > Bluetooth** (Samsung: **Settings > Connections > Bluetooth**) and stay on that screen.
2. On the Mac, open **System Settings > Bluetooth**. Your phone appears under **Nearby Devices**. Click **Connect**.
3. Both screens show the same number. Tap **Pair** on the phone and click **Connect** on the Mac.

---

## Everyday use

There is nothing to do. Here is what you will notice:

- **When Wi-Fi drops**, within about 20 seconds your Mac is online again through your phone, and a **hotspot icon** appears in the menu bar at the top of the screen.
- **While it uses your phone**, Bluetooth headphones, keyboards and mice on the Mac pause, and the Bluetooth switch shows **off**. This is normal: the Mac's Bluetooth is busy carrying your internet.
- **When Wi-Fi comes back**, it switches back by itself, the hotspot icon goes away, and your Bluetooth devices reconnect. Sound goes back to your headphones.

Keep the phone nearby, with Bluetooth on. It does not need to be unlocked once set up.

---

## Questions

**How do I know it is working right now?**
Look for the hotspot icon in the menu bar. If it is there, your Mac is using your phone's internet. Click it for details.

**Why did my headphones, mouse or keyboard stop working?**
While the Mac uses your phone, its Bluetooth is busy carrying your internet, so other Bluetooth devices pause. They come back by themselves when Wi-Fi returns. To get them back sooner, click the hotspot icon and choose **Switch Back to Wi-Fi**. Wired headphones and the built-in keyboard and trackpad always keep working.

**Why does the Bluetooth switch say Off?**
Same reason: the Mac's Bluetooth radio is on and busy carrying your internet. Leave the switch alone. Turning it on does not stop the internet; the Mac takes the radio back within a few seconds.

**Why is it slow?**
Bluetooth is slow: about 1 megabit per second, whatever your phone's own connection speed. It is fine for messages, email and web pages, but slow for video and big downloads. It is meant to keep you connected when Wi-Fi is gone.

**Does it use my phone's mobile data?**
Yes, the same as a hotspot. If your mobile plan does not allow hotspot use, your Mac connects to the phone but cannot reach the internet.

**It did not switch to my phone. What do I check?**
1. Is Wi-Fi really off or disconnected? If the Mac has another network, such as a network cable, it uses that instead.
2. Is the phone nearby with Bluetooth on?
3. Is **Bluetooth tethering** switched on on the phone? Some phones switch it off after a restart.
4. Still not working? See [Something went wrong](#something-went-wrong).

**What happens when I restart the Mac?**
Nothing changes. It starts by itself every time you log in.

**How do I pause it for a while?**
Open Terminal, type `bt-tether auto off` and press Return. To turn it back on, type `bt-tether auto on`.

**How do I update it?**
Paste the same line from Step 2 into Terminal again. It downloads the newest version and keeps your settings.

**How do I remove it?**
Open Terminal, type `bt-tether uninstall` and press Return. It asks for your Mac password once and removes everything. Then, on the phone, open Bluetooth settings, find **Mac Tether** and choose **Forget** (Samsung: **Unpair**).

**Do I need to keep Terminal open?**
Only during setup. Afterwards you can close it.

**Is it safe?**
It installs nothing hidden and changes nothing in macOS itself. Everything it adds is listed under [What it installs](#what-it-installs), and uninstalling removes all of it.

---

## Something went wrong

| What you see | What to do |
|---|---|
| "the phone refused tethering" or "does not offer Bluetooth tethering" | Switch on **Bluetooth tethering** on the phone (see above), then try again. |
| "did not answer over Bluetooth" | Keep the phone unlocked and next to the Mac, with Bluetooth on. If Terminal asked to use Bluetooth, click Allow. If it keeps happening, say **y** when the setup offers to redo the pairing. |
| The phone said the pairing with "Mac Tether" failed | On the phone, open Bluetooth settings, forget **Mac Tether**, then run the setup line again. |
| Bluetooth stays off and devices do not come back | Open Terminal, type `bt-tether stop` and press Return. Restarting the Mac also fixes it. |
| "Bluetooth Tether does not have Bluetooth access" | Open **System Settings > Privacy & Security > Bluetooth** and switch on **Bluetooth Tether**. |
| Anything else | Open Terminal, type `bt-tether doctor` and press Return. It checks everything and says what to fix. |

Still stuck? [Open an issue](https://github.com/anshukg/mac-bluetooth-tether/issues) and paste what `bt-tether doctor` printed. Leave out anything personal.

---

## For the technically curious

### Commands

| Command | What it does |
|---|---|
| `bt-tether status` | What is happening now |
| `bt-tether only` | Tethering on, checked, then Wi-Fi off |
| `bt-tether wifi` | Wi-Fi back on, then tethering off |
| `bt-tether start` / `stop` | Tethering on or off by hand |
| `bt-tether auto on` / `off` | Resume or pause the automatic switching |
| `bt-tether test` | A one-minute test with the phone that installs nothing |
| `bt-tether doctor` | Checks the Mac and the phone |
| `bt-tether repair-pairing` | Redoes the Mac's own pairing with the phone |
| `bt-tether uninstall` | Removes everything (`--keep-pairing` keeps the phone pairing) |

### Installing from a copy of this repository

```sh
git clone https://github.com/anshukg/mac-bluetooth-tether.git
cd mac-bluetooth-tether
./install.sh            # guided setup
./install.sh --check    # only checks this Mac and lists its phones; changes nothing
./install.sh --try      # checks, builds and tests with the phone; installs nothing
```

### What it installs

| Where | What |
|---|---|
| `~/Library/Application Support/bt-tether/` | The tools, settings, logs, and the pairing key for "Mac Tether" |
| `~/Library/Application Support/bt-tether-source/` | The downloaded source, when set up with the one-line command |
| `~/Library/LaunchAgents/com.anshu.bt-tether.auto.plist` | Login item that runs the automatic switching |
| `/Library/LaunchDaemons/com.anshu.bt-tether.net.plist` | Creates a virtual network port at startup |
| `/Library/LaunchDaemons/com.anshu.bt-tether.link.plist` | Starts or clears that port's address when tethering starts or stops |
| `/Library/Application Support/bt-tether/` | Root-owned copy of the script those two jobs run |
| `/usr/local/bin/bt-tether` | The `bt-tether` command |

At every startup, the network helper gives your user account access to the Mac's packet-capture devices (`/dev/bpf*`). The tool needs this to pass network traffic between the virtual port and the phone. Wireshark's installer makes the same change. It means programs you run could capture network traffic. Uninstalling returns those devices to root. No passwordless administrator rule is created.

### How it works

macOS cannot do Bluetooth tethering any more. Its Bluetooth service limits the kind of channel tethering uses to 1024 bytes per packet, and Android needs 1691.

While tethering is on, this tool pauses Apple's Bluetooth service and runs [BTstack](https://github.com/bluekitchen/btstack) on the Mac's Bluetooth chip directly, through the same shared-memory channels Apple's service uses. The tether has its own Bluetooth address and its own pairing ("Mac Tether"), so it never interferes with the Mac's normal pairing. Network traffic flows through a virtual Ethernet pair (`feth`), so macOS treats the phone like any other network.

When tethering ends, the chip gets its own address back, Apple's Bluetooth service resumes, and the devices and sound output that were in use are restored. A small login item decides when to switch: no other network and the phone nearby means tethering on; another network back means tethering off.

### Tested on

- **Verified:** MacBook Air (M4, Bluetooth chip BCM_4388C2), macOS 26.5.2, Samsung Galaxy Z Fold7.
- **Expected but untested:** other Apple silicon Macs with Broadcom PCIe Bluetooth. `./install.sh --try` shows whether yours works without installing anything.
- **Not supported:** Intel Macs.
- **Untested:** iPhones.
- **Not yet observed:** automatic start after a real restart, and behaviour across sleep and wake.

### Licence

The files in this repository are under the MIT licence; see [LICENSE](LICENSE). The build downloads BTstack, whose licence allows personal, non-commercial use only. The BTstack patch in `third_party/` falls under that licence too. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
