# CopyToPoints plugin folder init.py (installed as ~/.nuke/CopyToPoints/init.py)
# Adds the plugin build that matches the running Nuke version to the plugin path:
#   classic/nuke14/  CopyToPoints.dll, MultiplyCf.dll   (classic 3D system, Nuke 14.1)
#   usd/nuke14|15|16|17/ CopyToPointsUSD.dll            (USD PointInstancer, Nuke 14.1 preview / 15+)
import glob
import os
import shutil
import time

import nuke

# ============================================================================
# DIAGNOSTICS - set ONE folder here and everything you need after a crash lands
# in it:
#   <folder>/CopyToPoints_log.txt   flushed trace of every build of both nodes
#                                   (engine steps, guards, brush strokes) - the
#                                   last line shows where a crash happened
#   <folder>/crashdumps/            Nuke's own crash dumps (*.dmp / *.crash from
#                                   %TEMP% and %TEMP%/nuke) are COPIED here at
#                                   every start-up, so they are not lost with
#                                   the temp folder
#   <folder>/sessions.txt           one line per Nuke start (version, time)
# Leave empty ("") for no logging.  Nuke's own crash dumps are still written
# by Nuke to %TEMP%/nuke (only NUKE_TEMP_DIR, set before Nuke starts, moves them).
# ============================================================================
CTP_DIAG_DIR = ""          # e.g. r"C:/temp/nuke_diag"   or   os.path.join(os.path.expanduser("~"), "nuke_diag")


def _ctp_setup_diagnostics(folder):
    try:
        folder = os.path.abspath(os.path.expanduser(folder))
        if not os.path.isdir(folder):
            os.makedirs(folder)
        # plugin trace log (both nodes read CTP_LOG once, on first use)
        os.environ["CTP_LOG"] = os.path.join(folder, "CopyToPoints_log.txt").replace("\\", "/")
        # rescue Nuke's crash dumps from the temp folder(s)
        dumps = os.path.join(folder, "crashdumps")
        if not os.path.isdir(dumps):
            os.makedirs(dumps)
        temps = []
        for v in ("NUKE_TEMP_DIR", "TEMP", "TMP", "TMPDIR"):
            t = os.environ.get(v)
            if t:
                temps.append(t)
                temps.append(os.path.join(t, "nuke"))
        temps.append("/tmp")
        copied = 0
        for t in temps:
            if not os.path.isdir(t):
                continue
            for pat in ("*.dmp", "*.crash", "Nuke*crash*.txt", "nuke*crash*.log"):
                for f in glob.glob(os.path.join(t, pat)):
                    dst = os.path.join(dumps, os.path.basename(f))
                    try:
                        if not os.path.exists(dst) or os.path.getmtime(dst) < os.path.getmtime(f):
                            shutil.copy2(f, dst)
                            copied += 1
                    except Exception:
                        pass
        with open(os.path.join(folder, "sessions.txt"), "a") as fh:
            fh.write("%s  Nuke %s  pid %d  log=%s  dumps copied=%d\n" % (
                time.strftime("%Y-%m-%d %H:%M:%S"), nuke.NUKE_VERSION_STRING, os.getpid(), os.environ["CTP_LOG"], copied))
        nuke.tprint("CopyToPoints diagnostics -> %s (log: CopyToPoints_log.txt, crash dumps: crashdumps/)" % folder)
    except Exception as e:   # never break Nuke start-up over logging
        nuke.tprint("CopyToPoints diagnostics disabled: %s" % e)


if CTP_DIAG_DIR:
    _ctp_setup_diagnostics(CTP_DIAG_DIR)
elif os.environ.get("CTP_LOG"):
    nuke.tprint("CopyToPoints: CTP_LOG=%s (plugin trace log)" % os.environ["CTP_LOG"])

_here = os.path.dirname(os.path.abspath(__file__))
_major = nuke.NUKE_VERSION_MAJOR
_minor = nuke.NUKE_VERSION_MINOR


def _ctp_build_dir(kind):
    """The folder holding the build for THIS Nuke.

    The NDK is not compatible across minor versions - a 16.0 build refuses to
    load in 16.1, and in 17.1 a plugin that fails to load aborts start-up - so a
    folder named for the exact version wins.  The old major-only folders
    (usd/nuke17, classic/nuke14) are still used when there is no exact match, so
    existing installs keep working.
    """
    exact = os.path.join(_here, kind, "nuke%d.%d" % (_major, _minor))
    if os.path.isdir(exact):
        return exact
    legacy = os.path.join(_here, kind, "nuke%d" % _major)
    if os.path.isdir(legacy):
        return legacy
    return None


for _p in (_ctp_build_dir("classic"), _ctp_build_dir("usd")):
    if _p:
        nuke.pluginAddPath(_p)

# Icons live in one folder rather than a copy per version, and Nuke resolves a
# menu icon="Foo.png" against the plugin path - so the folder has to be on it.
# The 24px file is the icon; Nuke picks up the @2x twin beside it on a high-dpi
# display by itself.
_icons = os.path.join(os.path.dirname(os.path.abspath(__file__)), "icons")
if os.path.isdir(_icons):
    nuke.pluginAddPath(_icons)
