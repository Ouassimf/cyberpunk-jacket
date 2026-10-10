# PlatformIO post-build step: reports the size of firmware.bin and fails the
# build when it does not fit the app slot it is meant for.
#
# PlatformIO's own check adds up the sections of the ELF file; what has to fit
# in the slot is the image file, which is a little larger (headers, padding,
# checksum and hash). The limit is custom_max_image_bytes in platformio.ini.
import os

Import("env")  # noqa: F821  (provided by PlatformIO)


def check_image_size(source, target, env):
    limit = int(env.GetProjectOption("custom_max_image_bytes", "0"))
    path = str(target[0])
    size = os.path.getsize(path)
    if limit <= 0:
        print("Image: %s is %d bytes (no slot limit set for this environment)" % (os.path.basename(path), size))
        return 0
    print("Image: %s is %d bytes, %.1f %% of its %d-byte app slot (%d bytes free)"
          % (os.path.basename(path), size, 100.0 * size / limit, limit, limit - size))
    if size > limit:
        print("Error: the image is %d bytes larger than the app slot. It cannot be installed." % (size - limit))
        return 1
    return 0


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", check_image_size)  # noqa: F821
