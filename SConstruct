#!/usr/bin/env python
# Builds the TressFX GDExtension into addons/tressfx/bin/.
#   scons platform=linux target=template_debug
#   scons platform=linux target=template_release
# See README.md, "Building from source".

import os

env = SConscript("godot-cpp/SConstruct", {"api_version": "4.7"})

env.Append(CPPPATH=["src/"])
sources = Glob("src/*.cpp")

# In-editor class reference (doc_classes/*.xml), debug/editor builds only.
if env["target"] in ["editor", "template_debug"] and Glob("doc_classes/*.xml"):
    doc_data = env.GodotCPPDocData("src/gen/doc_data.gen.cpp", source=Glob("doc_classes/*.xml"))
    sources.append(doc_data)

bin_dir = "addons/tressfx/bin"
if env["platform"] == "macos":
    library = env.SharedLibrary(
        "{}/libtressfx.{}.{}.framework/libtressfx.{}.{}".format(
            bin_dir, env["platform"], env["target"], env["platform"], env["target"]
        ),
        source=sources,
    )
elif env["platform"] == "ios":
    suffix = ".simulator" if env["ios_simulator"] else ""
    library = env.StaticLibrary(
        "{}/libtressfx.{}.{}{}.a".format(bin_dir, env["platform"], env["target"], suffix),
        source=sources,
    )
else:
    library = env.SharedLibrary(
        "{}/libtressfx{}{}".format(bin_dir, env["suffix"], env["SHLIBSUFFIX"]),
        source=sources,
    )

env.NoCache(library)
Default(library)
