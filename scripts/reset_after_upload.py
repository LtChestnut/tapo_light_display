# PlatformIO extra script: reset the ESP32-S3 after upload.
#
# esptool's default "--after hard_reset" uses an RTC-watchdog reset on the S3's native
# USB-Serial/JTAG port. The USB device drops off the bus mid-reset, pySerial then throws
# "Cannot configure port" and PlatformIO reports the (successful) upload as FAILED.
# So we upload with "--after no_reset" and pulse RTS ourselves, which the USB-Serial/JTAG
# peripheral turns into a clean chip reset.
Import("env")  # noqa: F821  (provided by SCons)


def _reset(source, target, env):
    import time

    import serial  # pyserial ships with PlatformIO

    port = env.subst("$UPLOAD_PORT")
    if not port:
        print("[reset_after_upload] no upload port known; power-cycle the knob to run the new firmware")
        return
    try:
        with serial.Serial(port) as s:
            s.dtr = False
            s.rts = True
            time.sleep(0.1)
            s.rts = False
        print(f"[reset_after_upload] reset {port}")
    except Exception as exc:  # the device may already be re-enumerating
        print(f"[reset_after_upload] reset skipped ({exc}); power-cycle the knob if it doesn't restart")


env.AddPostAction("upload", _reset)  # noqa: F821
