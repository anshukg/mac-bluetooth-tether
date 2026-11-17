# Third-party notices

## BTstack

This project uses [BTstack](https://github.com/bluekitchen/btstack) by BlueKitchen GmbH. BTstack is not included in this repository. `scripts/build.sh` downloads it at the commit pinned in `third_party/btstack.lock` and applies `third_party/btstack-macos.patch`.

The patch makes three changes to BTstack:

1. It removes a duplicate buffer release in `rfcomm_send`.
2. It adds a way to adopt a Classic link that is already open.
3. It adds a way to start without resetting the controller.

The patch is a modification of BTstack, so it is distributed under BTstack's licence.

BTstack's licence permits redistribution and use only "solely for personal benefit and not for any commercial purpose or for monetary gain". Commercial use of BTstack needs a licence from BlueKitchen GmbH: see <https://bluekitchen-gmbh.com>. The full text is in the `LICENSE` file of the downloaded copy, in `.vendor/`.
