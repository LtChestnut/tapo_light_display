# PlatformIO pre-script: stamp the git commit into the build as GIT_HASH, e.g. "6a3a701",
# with a "+" suffix when the working tree has uncommitted changes.
import subprocess

Import("env")  # noqa: F821  (provided by SCons)


def _git(*args):
    try:
        return subprocess.check_output(["git", *args], cwd=env.subst("$PROJECT_DIR"),  # noqa: F821
                                       stderr=subprocess.DEVNULL, text=True).strip()
    except Exception:
        return ""


h = _git("rev-parse", "--short=7", "HEAD") or "nogit"
if _git("status", "--porcelain", "--untracked-files=no"):
    h += "+"
env.Append(CPPDEFINES=[("GIT_HASH", env.StringifyMacro(h))])  # noqa: F821
print(f"[git_version] GIT_HASH={h}")
