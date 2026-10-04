"""PlatformIO hook: check the app's room in its flash slot after every build.

The sdkconfig check runs from CMakeLists.txt, which PlatformIO configures too.
"""

import os
import subprocess

Import("env")  # noqa: F821 - provided by SCons

_partitions = os.path.join("$BUILD_DIR", "partitions.bin")
_firmware = os.path.join("$BUILD_DIR", "${PROGNAME}.bin")


def _check_size(target, source, env):
    script = os.path.join(env.subst("$PROJECT_DIR"), "tools", "check_size.py")
    return subprocess.call([env.subst("$PYTHONEXE"), script, "--app", str(target[0]),
                            "--partitions", env.subst(_partitions)])


env.Depends(_firmware, _partitions)  # noqa: F821
env.AddPostAction(_firmware, _check_size)  # noqa: F821
