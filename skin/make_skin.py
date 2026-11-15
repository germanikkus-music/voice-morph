"""Builds Voice Morph's skins/ folder: the ui.json layout from tools/gen_ui.py with
skin/backdrop.png as the page background. The SDK's generator has no background-image
option, so this runs it and then sets the image on every tab.

Run after changing ui.json or the backdrop, after one normal build (it needs build/params.json):

    python "plugins/Voice Morph/skin/make_skin.py"
    python tools/build.py "plugins/Voice Morph"

build.py then copies skins/ verbatim instead of generating the UI.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
SDK = os.path.dirname(os.path.dirname(PROJECT))


def main():
    params = os.path.join(PROJECT, "build", "params.json")
    if not os.path.isfile(params):
        sys.exit("build/params.json is missing: run tools/build.py once first")
    tmp = tempfile.mkdtemp()
    try:
        subprocess.check_call([sys.executable, os.path.join(SDK, "tools", "gen_ui.py"), "--params", params,
                               "--ui", os.path.join(PROJECT, "ui.json"), "--out", tmp,
                               "--kit-cache", os.path.join(PROJECT, "build")])
        tui_path = os.path.join(tmp, "TUI.json")
        with open(tui_path) as f:
            tui = json.load(f)
        for tab in tui["pageData"]["tabs"]:
            bg = tab["componentDefinition"]["backgroundData"]
            for state in ("focussed", "unfocussed"):
                bg[state]["colour"] = "ff0a0e1c"
                bg[state]["image"] = "backdrop.png"
        with open(tui_path, "w") as f:
            json.dump(tui, f, indent=4)
        shutil.copy(os.path.join(HERE, "backdrop.png"), tmp)
        out = os.path.join(PROJECT, "skins")
        if os.path.isdir(out):
            shutil.rmtree(out)
        shutil.copytree(tmp, out)
        print("wrote", out)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    main()
