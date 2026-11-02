d/wmr: Recover the companion device after a USB re-enumeration: back off the
companion read on a sustained dropout (all platforms), then, on Linux, reopen
the new hidraw node and re-assert the panel state (`WMR_COMPANION_RECONNECT=0`
disables the reopen).
