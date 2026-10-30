d/wmr: Make `wmr_camera_stop()` wait for the camera's in-flight image transfers
to finish before returning, fixing a SIGSEGV in the USB thread on session
teardown. Also drop a trailing wait in the camera USB thread that could hang
the `wmr_camera_free()` join forever.
