# Display calibration reference

`accepted_display_spike.c` is a standalone, credential-free source snapshot used
by `firmware/main/test_physical_calibration.py`. Its byte length and SHA-256 are
checked against the calibration manifest. It is not compiled into the firmware.

Keeping this reference in the tree makes the tests reproducible from a fresh
clone without importing the original project's private Git history.
