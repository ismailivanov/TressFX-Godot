# TressFX for Godot

Real-time hair and fur for Godot 4.7, ported from [AMD TressFX 4.1](https://github.com/GPUOpen-Effects/TressFX)
as a native GDExtension. This guide explains how to install the addon, bring in a hairstyle, make
it collide with a character, tune it, and keep it fast.

| Page | What it covers |
|---|---|
| [Getting Started](Getting-Started.md) | Install, project settings, your first hair, colliders, the editor |
| [Hair from Blender](Blender.md) | Grooming hair in Blender, exporting it, setting it up in Godot |
| [Assets](Assets.md) | The `.tfx`, `.tfxbone` and `.tfxmesh` formats and how to produce them |
| [Nodes](Nodes.md) | Every property and method of `TressFXHair`, `TressFXCollisionMesh`, `TressFXStats` |
| [Tuning](Tuning.md) | What the simulation parameters do, recipes for fur, long hair and ponytails, fixing jitter |
| [Performance](Performance.md) | What costs what, measured numbers, LOD, shadows, memory |
| [Troubleshooting](Troubleshooting.md) | Nothing shows up, hair explodes, hair goes through the body, editor issues |
| [Building](Building.md) | Building the extension from source, CI, code layout |

## Requirements

- Godot 4.7 or later.
- The **Forward+** or **Mobile** renderer. The Compatibility renderer has no `RenderingDevice`
  and cannot run the compute shaders.
- A GPU with Vulkan, Direct3D 12 or Metal support (anything that runs Forward+).
- No special project settings; TAA is recommended (see [Getting Started](Getting-Started.md)).

## How it works, in one paragraph

A hairstyle is a set of *guide strands*, each a chain of 4 to 64 vertices. Every frame a compute
shader moves the guide vertices: it applies gravity and wind, pulls each strand back towards its
authored shape, keeps every segment at its rest length, and copies the motion of the skeleton
bone the strand is attached to. Colliders are turned into a signed distance field (a 3D grid of
"how far from the surface am I") and any vertex that ends up inside is pushed out. Around each
guide strand a few *follow strands* are placed as offset copies, which is where the visual density
comes from. Finally every strand is drawn as a thin camera-facing ribbon whose width is expanded
to at least a pixel, with a lighting model made for fibers.
