# usbhost tests

The host window takes the USB PHY away from USB-Serial/JTAG, so one ewt run
can't straddle it. Sequence (watch on the Mac's USB, nothing else attached):

1. `ewt flash usbhost`
2. `ewt run test/enter.ewt`  -> enters the screen, passes before the port drops
3. wait ~35 s (port disappears ~1.5 s after entering, comes back ~31 s later)
4. `ewt run test/after.ewt`  -> still on UsbHost, serial back, snap of results
5. `ewt raw "log dump" -t 3 | grep USBH` -> scan start / host up / host down lines

Step 4 passing is the important check: it proves the PHY goes back to
Serial/JTAG without a reset. If the port never comes back, power-cycle the watch
(the boot ROM restores Serial/JTAG) and report it.

Keyboard test (Ewan): unplug the Mac, plug the hub/adapter into the watch, open
USB Host (or tap to rescan), read the list. Re-plug the Mac and `log dump` for
the full USBH lines (VID:PID, class, interfaces).
