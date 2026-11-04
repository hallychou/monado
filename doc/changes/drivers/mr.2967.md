d/wmr: Survive transient HID errors instead of failing the session: companion
device drops, controller firmware read timeouts and stale replies, and read
errors in the Bluetooth controller thread. Also bound the startup wait for
controller status.
