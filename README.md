# FakeMIOS

Small UEFI application for experimenting with the MIOS ecosystem.

The current version focuses on a shell-like interface, `MIIMG` image rendering, `MIGIF` animation playback, splash screens, and a Secure Boot-oriented boot layout built around shim.

The repository is prepared for public sharing:

- source code, docs, source assets, and canonical boot helpers are kept in Git
- private signing material under `keys/` is intentionally ignored
- generated build outputs, staged assets, and machine-local runtime state are intentionally ignored

## Current features

- shell-style command interface inside a UEFI application
- `MIIMG` decoding and centered image rendering
- `MIGIF` parsing, caching, playback, and splash rendering
- Secure Boot-oriented boot flow using shim and MokManager
- local QEMU and USB workflows through the `Makefile`

## Requirements

- `gcc`
- `binutils` (`ld`, `objcopy`)
- `gnu-efi`

For Secure Boot signing and local QEMU boot testing, you also need:

- `sbsigntool` (`sbsign`)
- `qemu-system-x86_64`
- `ovmf`

On Debian/Ubuntu-like systems, a typical package set is:

```bash
sudo apt update
sudo apt install build-essential binutils gnu-efi sbsigntool qemu-system-x86 ovmf
```

## Build and signing

### Unsigned build

This works from a fresh clone and does not require anything inside `keys/`:

```bash
make
```

That produces a local unsigned `grubx64.efi` in the repository root.

### Secure Boot staging

Secure Boot-related staging remains local-only and requires private key material:

```bash
make copy
```

This target:

- adds the SBAT section
- signs `grubx64.efi`
- copies the certificate export to `image/boot_keys/`
- stages the signed binary into `image/EFI/BOOT/`
- mirrors source assets into `image/EFI/BOOT/assets/`

If the required files are missing, the target fails with a clear error instead of producing a half-configured boot image.

### Release archive

To create a GitHub Releases-ready archive of the staged boot image:

```bash
make release
```

This produces:

- `dist/fake-mios-image.zip`

The archive contains the staged `image/` directory and excludes the local `image/NvVars` runtime state file.

## Local key setup

The `keys/` directory is intentionally ignored by Git. Create it locally and generate your own key pair and certificate.

```bash
mkdir -p keys

openssl req \
  -new -x509 -newkey rsa:2048 -sha256 \
  -keyout keys/mios.key \
  -out keys/mios.crt \
  -days 3650 \
  -nodes \
  -subj "/C=US/ST=State/L=City/O=MIOS/OU=MIOS Dev/CN=MIOS"
```

Export the certificate in DER form for enrollment:

```bash
openssl x509 -in keys/mios.crt -outform DER -out keys/mios.der
```

Create `keys/sbat.csv` with at least:

```csv
sbat,1,SBAT Version,sbat,1,https://github.com/rhboot/shim/blob/main/SBAT.md
mios,1,Mihior,MIOS,1,https://example.com
```

The `Makefile` expects these local files:

- `keys/mios.key`
- `keys/mios.crt`
- `keys/mios.der`
- `keys/sbat.csv`

## Running locally

The default QEMU target is:

```bash
make run
```

Important notes:

- `make run` expects a usable boot image under `image/`
- for a Secure Boot boot path, you normally want to stage a signed `grubx64.efi` first with `make copy`
- `OVMF_VARS_4M.ms.fd` is local writable runtime state and is not tracked in Git
- if you need a fresh local vars file, create it manually before running QEMU:

```bash
cp /usr/share/OVMF/OVMF_VARS_4M.ms.fd OVMF_VARS_4M.ms.fd
```

The USB workflow is also local-only:

```bash
make usb
```

It checks that drive `E:` exists and has the label `MIOS` before touching it, and it also depends on the local Secure Boot signing material.

## Installing on a USB drive

For a manual USB installation:

1. Format the USB drive as `FAT32`.
2. Build and stage the boot image:

```bash
make release
```

3. Open the generated `image/` directory.
4. Copy everything inside `image/` to the root of the USB drive.

Important:

- copy the contents of `image/`, not the `image` folder itself
- after copying, the USB root should contain `EFI/` and `boot_keys/`
- the release archive `dist/fake-mios-image.zip` follows the same layout

If Secure Boot is enabled on the target machine, the user also needs to trust the local signing certificate:

- use `boot_keys/mios.der` from the prepared image
- import or enroll that certificate through the firmware Secure Boot interface or through MokManager, depending on the platform
- after the certificate is trusted, shim can continue to `grubx64.efi`

Without certificate enrollment, Secure Boot systems may refuse to launch the signed FakeMIOS loader.

## Boot layout

The canonical boot helper binaries stored in the repository are:

- `image/EFI/BOOT/BOOTX64.EFI`: shim
- `image/EFI/BOOT/mmx64.efi`: MokManager

The FakeMIOS application itself is built as `grubx64.efi`.

In the Secure Boot flow, shim loads `grubx64.efi`. If you use your own signing certificate, you will usually need to enroll the certificate or hash through MokManager, depending on how you test and boot.

## Project structure

```text
fakeos/
+-- docs/
|   `-- MIGIF_decoder_full_support_spec.txt
+-- image/
|   `-- EFI/BOOT/
|       +-- BOOTX64.EFI
|       `-- mmx64.efi
+-- src/
|   +-- assets/
|   `-- main.c
+-- .gitignore
+-- Makefile
`-- README.md
```

## License

This project is licensed under the MIT License. See the `LICENSE` file for details.
