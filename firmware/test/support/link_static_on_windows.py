# PlatformIO extra script for [env:native].
#
# On Windows a MinGW-built program loads libstdc++-6.dll, libgcc_s_seh-1.dll and
# libwinpthread-1.dll from whichever PATH directory has them first, and other
# software (GTK, Git, the Android SDK, ...) ships older copies. The test
# programs then die at start-up with 0xC0000139 before running a single test.
# Linking the runtime in statically makes them independent of PATH.
import sys

Import("env")  # noqa: F821  (provided by PlatformIO)

if sys.platform == "win32":
    env.Append(LINKFLAGS=["-static"])  # noqa: F821
