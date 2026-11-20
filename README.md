# Helett H30C Lite — Linux CUPS driver

Open-source Linux (CUPS) driver for the **Helett H30C Lite** USB 4×6 direct-thermal
shipping label printer. The vendor only ships Windows and macOS drivers.

The printer identifies itself as a **4BARCODE 4B-2054N** (USB ID `2d84:b028`) and speaks
**TSPL2** at 203 dpi, so this driver will likely also work for other 4BARCODE/TSPL rebrands
with small PPD tweaks.

Combined with CUPS printer sharing and Avahi, it turns the USB-only printer into a
network **AirPrint / IPP Everywhere** printer that phones and laptops find automatically,
with no driver needed on the clients (Windows: use the built-in "Microsoft IPP Class Driver").

## Files

| File | Purpose |
|---|---|
| `rastertotspl.c` | CUPS filter: 8-bit grayscale (or 1-bit) raster → TSPL. Converts to black & white with a sharp threshold (crisp text and barcodes) or Floyd–Steinberg dithering (photos). |
| `helett-h30c.ppd` | Printer description: label sizes (4×6 default, 4×8, 4×4, 4×3, 4×2, 3×2, 2×4, 2.25×1.25, 2×1, 1.5×1, 100×150 mm, custom), darkness, speed, media type, rotate 180°, B&W conversion, auto-crop. |
| `labelcrop` | Optional CUPS prefilter (Python). Detects **iCarry** shipping labels, which come as an A4 page with the label in the top part, and crops, rotates and scales them to fill the 4×6 label. All other documents pass through untouched. |
| `test-4x6.ps` | Test label: `ps2pdf test-4x6.ps && lp -d Helett_H30C test-4x6.pdf` |
| `host/` | Optional: udev rule, `usblp` blacklist and LXC config for running CUPS inside an unprivileged Proxmox LXC container. |

## Install (Debian / Ubuntu)

```sh
sudo apt install cups cups-filters avahi-daemon libcups2-dev libcupsimage2-dev gcc \
                 python3-pypdf python3-pil poppler-utils

gcc -O2 -Wall -Wno-deprecated-declarations -o rastertotspl rastertotspl.c -lcupsimage -lcups
sudo install -m 755 rastertotspl labelcrop /usr/lib/cups/filter/

lpinfo -v | grep usb     # e.g. usb://4BARCODE/4B-2054N?serial=XXXXXXXX
sudo lpadmin -p Helett_H30C -E -v 'usb://4BARCODE/4B-2054N?serial=XXXXXXXX' \
             -P helett-h30c.ppd -o printer-is-shared=true
sudo lpadmin -d Helett_H30C

# Share on the network (AirPrint / IPP Everywhere / Mopria)
sudo cupsctl --share-printers --remote-any
sudo systemctl enable --now avahi-daemon
```

If you don't need iCarry cropping, skip `labelcrop` and delete the `*cupsPreFilter` line
from the PPD.

### Running inside a Proxmox LXC

CUPS talks to the printer through libusb, so the container needs the USB bus. On the
Proxmox **host**:

```sh
cp host/99-labelprinter.rules /etc/udev/rules.d/
cp host/labelprinter-usblp.conf /etc/modprobe.d/     # keep usblp from grabbing the printer
rmmod usblp; udevadm control --reload && udevadm trigger
cat host/lxc-passthrough.conf >> /etc/pve/lxc/<CTID>.conf
pct reboot <CTID>
```

## Print options

Set these in the print dialog or per job with `lp -o`:

| Option | Values | Default |
|---|---|---|
| `PageSize` | `w288h432` (4×6) and others in the PPD | 4×6 |
| `Halftone` | `128` (normal), `170` (bold), `210` (extra bold), `Dither` | `170` |
| `Darkness` | `-1` (printer setting), `0`–`15` | printer setting |
| `PrintSpeed` | `Normal` (printer setting), `2`–`6` ips | printer setting |
| `PaperType` | `Normal`, `LabelGaps`, `LabelMark`, `Continue` | printer setting |
| `Rotate180` | `True` / `False` | `False` |
| `AutoCrop` | `True` / `False` (iCarry labels only) | `True` |

## How it works

```
PDF from client → CUPS → labelcrop (iCarry only) → pdftopdf (fit to label)
  → gstoraster (203 dpi, 8-bit gray) → rastertotspl → USB → printer
```

`rastertotspl` sends one TSPL job per label:

```
SIZE 101.6 mm,152.4 mm
DIRECTION 1
REFERENCE 0,0
CLS
BITMAP 0,0,<bytes per row>,<rows>,0,<1-bit bitmap, 0 = black dot>
PRINT 1,<copies>
```

## Notes

- Not affiliated with Helett or 4BARCODE. Option ranges and label sizes were derived
  from the vendor's macOS printer description file for the 4B-2054N. No vendor code is included.
- Calibrate the printer's label sensor once, with its front button, after loading a new roll.

## License

MIT
