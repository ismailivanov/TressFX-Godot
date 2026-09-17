# Building

The extension is C++ built against [godot-cpp](https://github.com/godotengine/godot-cpp) (v10,
`api_version=4.7`) with SCons. You need a C++17 compiler, Python 3 and SCons, the same
prerequisites as for building Godot itself.

```bash
git clone --depth 1 https://github.com/godotengine/godot-cpp godot-cpp
scons platform=linux target=template_debug
scons platform=linux target=template_release
```

`platform` is `linux`, `windows` or `macos` (add `arch=universal` on macOS; `arch=arm64` for
Linux on ARM). The first build compiles godot-cpp and takes a few minutes; later builds only
compile `src/`. The libraries are written to `addons/tressfx/bin/` with the names listed in
`addons/tressfx/tressfx.gdextension`. `template_debug` is what the editor and debug exports load,
`template_release` what release exports load.

The GitHub Actions workflow in `.github/workflows/build.yml` builds Linux, Windows and macOS on
every push and uploads the binaries and a packaged addon as artifacts.

## Layout

- `src/` the sources. `tressfx_hair` and `tressfx_collision_mesh` each have a main-thread node
  and a render-thread `*GPU` object that owns the `RenderingDevice` resources; `tressfx_asset`
  loads `.tfx` and `.tfxbone`; `tressfx_skin` builds the skinning matrices; `tressfx_gpu` holds
  the compute pipelines shared by every node; `tressfx_stats` is the overlay; `tressfx_editor` is
  the always-on editor plugin that draws the collider gizmos (registered at the editor
  initialization level only, so exported games never load it).
- `addons/tressfx/shaders/` the compute shaders (`tressfx_sim.glsl` simulation,
  `tressfx_sdf.glsl` distance field build, `tressfx_sdf_collide.glsl` collision) and the strand
  material shader. They are Godot `.glsl` resources compiled to SPIR-V by the editor's importer.
- `doc_classes/` the in-editor class reference, compiled into debug builds by the SConstruct.
- `upstream/` the untouched AMD TressFX 4.1 sources the port was made from.

Code style follows the Godot engine; `.clang-format` is in the repository root.

## Threading

Everything that touches the `RenderingDevice` runs through
`RenderingServer.call_on_render_thread()`. Each node hands a heap-allocated GPU object to the
render thread and never touches its members again; freeing is queued the same way, so the order
of creation, use and destruction is the order of the command queue whatever the project's
render thread model.
