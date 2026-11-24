"""Builds a Windows VST2 of an SDK plugin WITH an editor, for trying it in Ableton or any VST2 host.

    python build_gui.py <PluginName> [<PluginName> ...]     (or: all)

The plugin's own source and the SDK are not modified: the editor is added through shim/mpc_plugin.h
(see af_editor_win.h). The layout comes from plugins/<Name>/ui.json. The result is a folder
"<Name> GUI" holding "<slug>-gui.dll", plugin.json and Content/, ready to copy next to the other
VST2 plugins. It is built beside, not over, the plain version.
"""
import glob
import json
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
# The MPC plugin builder SDK, with these plugins copied into its plugins/ folder (see the README).
# Set MPC_SDK to its folder; by default a folder named mpc-plugin-builder-sdk next to this repo is used.
SDK = os.environ.get("MPC_SDK") or os.path.join(HERE, "..", "..", "mpc-plugin-builder-sdk")
if not os.path.isdir(os.path.join(SDK, "tools")):
    sys.exit("MPC SDK not found at %s: set the MPC_SDK environment variable to its folder" % os.path.abspath(SDK))
SDK = os.path.abspath(SDK)
sys.path.insert(0, os.path.join(SDK, "tools"))
sys.path.insert(0, HERE)

import build as sdk          # noqa: E402  (the SDK's own helpers: zig lookup, flags, slug)
import gen_layout            # noqa: E402


def main(name, out_root=None, accent=None):
    project = os.path.join(SDK, "plugins", name)
    manifest = json.load(open(os.path.join(project, "plugin.json"), encoding="utf-8"))
    out_root = out_root or os.path.join(HERE, "out")
    gen_dir = os.path.join(out_root, "_gen", name)
    folder = os.path.join(out_root, name + " GUI")
    dll = os.path.join(folder, sdk.make_slug(manifest["name"]) + "-gui.dll")

    gen_layout.generate(os.path.join(project, "ui.json"), manifest["name"], gen_dir, accent)
    os.makedirs(folder, exist_ok=True)

    sources = sorted(glob.glob(os.path.join(project, "src", "*.cpp")))
    argv = (sdk.find_zig(None) + ["c++"] + list(sdk.COMMON_FLAGS) + ["-O2"]
            + ["-I" + os.path.join(HERE, "shim"), "-I" + HERE, "-I" + gen_dir,
               "-I" + sdk.FRAMEWORK_DIR, "-I" + os.path.join(project, "src"), "-DAUDIOFEEL_EDITOR"]
            + sdk.native_target_flags_for_zig() + ["-o", dll] + sources
            + ["-luser32", "-lgdi32", "-lgdiplus"])
    code, out = sdk.run(argv)
    if out.strip():
        print(out.rstrip())
    if code != 0 or not os.path.isfile(dll):
        sys.exit("build failed (exit code %s)" % code)
    for extra in glob.glob(os.path.join(folder, "*.pdb")) + glob.glob(os.path.join(folder, "*.lib")):
        os.remove(extra)  # linker leftovers, not needed by a host
    shutil.copyfile(os.path.join(project, "plugin.json"), os.path.join(folder, "plugin.json"))
    sdk.copy_content(project, folder)
    print("built: %s (%d KB)" % (dll, os.path.getsize(dll) // 1024))
    return folder


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    names = sys.argv[1:]
    if names == ["all"]:
        names = sorted(d for d in os.listdir(os.path.join(SDK, "plugins"))
                       if os.path.isfile(os.path.join(SDK, "plugins", d, "ui.json")))
    for n in names:
        main(n)
