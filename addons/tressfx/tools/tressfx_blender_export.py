# SPDX-License-Identifier: MIT
"""Blender to Godot Hair: grooms hair in Blender and exports it to TressFX for Godot (.tfx, .tfxbone,
.tfxmesh colliders).

Install: Edit > Preferences > Add-ons > (menu) Install from Disk, pick this file.
Use: 3D Viewport > Sidebar (N) > TressFX, or File > Export > TressFX Hair (.tfx).

Positions are written in Blender world space turned Y-up, the space Blender's glTF exporter uses
for skinned meshes, so the hair lines up with the character once both are in Godot. Strand UVs
and bone weights come from the closest point on the hair's surface mesh. Meshes marked as hair
colliders are written as .tfxmesh files, skinned the same way, and left out of the .glb.
"""

bl_info = {
    "name": "Blender to Godot Hair",
    "author": "Ismail Ivanov",
    "version": (0, 2, 0),
    "blender": (4, 2, 0),
    "location": "3D Viewport > Sidebar > TressFX, File > Export > TressFX Hair (.tfx)",
    "description": "Groom hair in Blender and export it to Godot with TressFX (.tfx, .tfxbone, .tfxmesh, .glb)",
    "doc_url": "https://github.com/ismailivanov/Blender-to-Godot-Hair",
    "category": "Import-Export",
}

import contextlib
import os
import struct

import bpy
import numpy as np
from bpy.props import BoolProperty, EnumProperty, PointerProperty, StringProperty
from bpy_extras.io_utils import ExportHelper
from mathutils import Vector
from mathutils.bvhtree import BVHTree
from mathutils.geometry import intersect_point_line
from mathutils.interpolate import poly_3d_calc

VERTEX_COUNTS = [(str(n), str(n), "") for n in (4, 8, 16, 32, 64)]
try:  # Blender 4.5+ only allows "//" (next to the .blend) in path properties that opt in.
    bpy.props.StringProperty(options={'PATH_SUPPORTS_BLEND_RELATIVE'})
    PATH_OPTIONS = {'PATH_SUPPORTS_BLEND_RELATIVE'}
except TypeError:
    PATH_OPTIONS = set()
VERTEX_COUNT_HELP = ("Every curve is resampled to this many points. 8 for short fur, 16 for most hair, "
                     "32 for long or curly hair")


def _rig(surface):
    """The armature that moves the surface, and the bone it hangs from when it is not skinned."""
    armature = surface.find_armature()
    if armature:
        return armature, None
    o = surface
    while o.parent:
        if o.parent_type == 'BONE' and o.parent.type == 'ARMATURE':
            return o.parent, o.parent_bone
        o = o.parent
    return None, None


@contextlib.contextmanager
def _rest_pose(context, armature):
    """Evaluate the scene with the armature in its rest position (the pose the glTF file binds)."""
    old = armature.data.pose_position if armature else None
    if armature:
        armature.data.pose_position = 'REST'
    try:
        context.view_layer.update()
        yield context.evaluated_depsgraph_get()
    finally:
        if armature:
            armature.data.pose_position = old
            context.view_layer.update()


def _godot_bone_name(name):
    # Godot's glTF importer replaces ':' and '/' in bone names.
    return name.replace(":", "_").replace("/", "_")


def _top_bones(weights):
    """The four strongest (bone, weight) pairs, normalized; empty when there is no weight."""
    top = sorted(weights.items(), key=lambda kv: -kv[1])[:4]
    total = sum(w for _, w in top)
    return [(name, w / total) for name, w in top] if total > 0.0 else []


def _world_curves(obj_eval):
    """The evaluated hair curves, one (points, 3) world-space array per curve, root first."""
    curves = obj_eval.data
    if len(curves.curves) == 0:
        return []
    pos = np.empty(len(curves.points) * 3, dtype=np.float64)
    curves.position_data.foreach_get("vector", pos)
    pos = pos.reshape(-1, 3)
    m = np.array(obj_eval.matrix_world)
    pos = pos @ m[:3, :3].T + m[:3, 3]
    offsets = np.empty(len(curves.curves) + 1, dtype=np.int32)
    curves.curve_offset_data.foreach_get("value", offsets)
    return [pos[a:b] for a, b in zip(offsets[:-1], offsets[1:])]


def _resample(pts, n):
    """n points evenly spaced along the polyline."""
    dist = np.concatenate(([0.0], np.cumsum(np.linalg.norm(np.diff(pts, axis=0), axis=1))))
    t = np.linspace(0.0, dist[-1], n)
    return np.stack([np.interp(t, dist, pts[:, k]) for k in range(3)], axis=1)


def _nearest_bone(armature, p):
    best = (float("inf"), None)
    for bone in armature.data.bones:
        if not bone.use_deform:
            continue
        head = armature.matrix_world @ bone.head_local
        tail = armature.matrix_world @ bone.tail_local
        q, t = intersect_point_line(p, head, tail)
        q = head if t < 0.0 else tail if t > 1.0 else q
        best = min(best, ((q - p).length, bone.name))
    return best[1]


def _bind_roots(roots, surface_eval, armature, parent_bone, uv_name):
    """Per root: its surface UV (V down, as in Godot) and up to four (bone name, weight) pairs."""
    mesh = surface_eval.to_mesh()
    try:
        mesh.calc_loop_triangles()
        tris = mesh.loop_triangles
        bvh = BVHTree.FromPolygons([v.co for v in mesh.vertices], [t.vertices for t in tris])
        uv_layer = mesh.uv_layers.get(uv_name) or mesh.uv_layers.active
        deform = {b.name for b in armature.data.bones if b.use_deform} if armature else set()
        group_names = [g.name for g in surface_eval.vertex_groups]
        to_local = surface_eval.matrix_world.inverted()
        uvs, bindings, unweighted = [], [], 0
        for root in roots:
            loc, _, ti, _ = bvh.find_nearest(to_local @ Vector(root))
            tri = tris[ti]
            bary = poly_3d_calc([mesh.vertices[v].co for v in tri.vertices], loc)
            if uv_layer:
                uv = sum((b * uv_layer.uv[l].vector for b, l in zip(bary, tri.loops)), Vector((0.0, 0.0)))
                uvs.append((uv.x, 1.0 - uv.y))
            if not armature:
                continue
            if parent_bone:  # A rigid part of the character: vertex groups do not move it.
                bindings.append([(parent_bone, 1.0)])
                continue
            acc = {}
            for b, v in zip(bary, tri.vertices):
                for g in mesh.vertices[v].groups:
                    name = group_names[g.group] if g.group < len(group_names) else None
                    if name in deform and g.weight > 0.0:
                        acc[name] = acc.get(name, 0.0) + b * g.weight
            top = _top_bones(acc)
            if not top:
                unweighted += 1
                top = [(_nearest_bone(armature, Vector(root)), 1.0)]
            bindings.append(top)
        return (uvs if uv_layer else None), bindings, unweighted
    finally:
        surface_eval.to_mesh_clear()


def _write_tfx(path, strands, uvs):
    s, n, _ = strands.shape
    data = np.zeros((s, n, 4), dtype="<f4")
    # Z-up to Y-up, the same swap the glTF exporter does.
    data[..., 0] = strands[..., 0]
    data[..., 1] = strands[..., 2]
    data[..., 2] = -strands[..., 1]
    data[:, 2:, 3] = 1.0  # Inverse mass: the first two vertices of every strand are pinned.
    uv_offset = 160 + data.nbytes if uvs else 0
    with open(path, "wb") as f:
        # version, strands, vertices per strand, position / strand UV / vertex UV / thickness /
        # color offsets, then 32 reserved uint32: a 160-byte header.
        f.write(struct.pack("<fIIIIIII", 4.0, s, n, 160, uv_offset, 0, 0, 0) + bytes(128))
        f.write(data.tobytes())
        if uvs:
            f.write(np.asarray(uvs, dtype="<f4").tobytes())


def _write_tfxbone(path, bindings):
    names = sorted({name for b in bindings for name, _ in b})
    index = {name: i for i, name in enumerate(names)}
    with open(path, "wb") as f:
        f.write(struct.pack("<I", len(names)))
        for i, name in enumerate(names):
            raw = _godot_bone_name(name).encode("utf-8")
            f.write(struct.pack("<II", i, len(raw) + 1) + raw + b"\0")
        f.write(struct.pack("<I", len(bindings)))
        for s, b in enumerate(bindings):
            f.write(struct.pack("<I", s))
            for k in range(4):
                name, w = b[k] if k < len(b) else (b[0][0], 0.0)
                f.write(struct.pack("<If", index[name], w))


def export_hair(context, obj, path, verts, bones):
    """Writes the hair object to path (.tfx) and its .tfxbone.

    Returns (report type, message); the message is short parts joined by "; " for the panel.
    """
    surface = obj.data.surface
    armature, parent_bone = _rig(surface) if surface and bones else (None, None)
    with _rest_pose(context, armature) as dg:
        curves = [c for c in _world_curves(obj.evaluated_get(dg)) if len(c) >= 2 and np.ptp(c, axis=0).any()]
        if not curves:
            return 'ERROR', f"{obj.name}: no curves; plant hair with the Add brush"
        strands = np.array([_resample(c, verts) for c in curves])
        uvs, bindings, unweighted = None, [], 0
        if surface:
            uvs, bindings, unweighted = _bind_roots(strands[:, 0], surface.evaluated_get(dg), armature,
                                                    parent_bone, obj.data.surface_uv_map)

    _write_tfx(path, strands, uvs)
    msg = f"{os.path.basename(path)}: {len(strands)} strands"
    if armature:
        _write_tfxbone(os.path.splitext(path)[0] + ".tfxbone", bindings)
        if parent_bone:
            msg += f"; follows bone {parent_bone}"
        else:
            msg += f"; skinned to {len({name for b in bindings for name, _ in b})} bones"
    if not surface:
        return 'WARNING', f"{msg}; no surface: no UVs, no bones"
    if unweighted:
        return 'WARNING', f"{msg}; {unweighted} roots without weights; they follow the nearest bone"
    return 'INFO', msg


def _write_tfxmesh(path, points, bindings, triangles):
    names = sorted({name for b in bindings for name, _ in b})
    index = {name: i for i, name in enumerate(names)}
    lines = ["# TressFX collision mesh exported by Blender to Godot Hair",
             f"numOfBones {len(names)}", "# bone index, bone name"]
    lines += [f"{i} {_godot_bone_name(name)}" for i, name in enumerate(names)]
    lines += [f"numOfVertices {len(points)}",
              "# vertex index, position x y z, normal x y z, joint index 0-3, weight 0-3"]
    for i, ((p, n), b) in enumerate(zip(points, bindings)):
        joints = [index[name] for name, _ in b] + [0] * (4 - len(b))
        weights = [w for _, w in b] + [0.0] * (4 - len(b))
        if not b:
            weights[0] = 1.0  # No armature: everything on the node's own transform.
        # Z-up to Y-up, like the positions in the .tfx.
        lines.append(f"{i} {p.x:.6g} {p.z:.6g} {-p.y:.6g} {n.x:.6g} {n.z:.6g} {-n.y:.6g} "
                     + " ".join(map(str, joints)) + " " + " ".join(f"{w:.6g}" for w in weights))
    lines += [f"numOfTriangles {len(triangles)}", "# triangle index, vertex index 0 1 2"]
    lines += [f"{i} {a} {b} {c}" for i, (a, b, c) in enumerate(triangles)]
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def export_collider(context, obj, path, bones):
    """Writes a mesh to path as a .tfxmesh collider, in rest pose and skinned like the hair."""
    armature, parent_bone = _rig(obj) if bones else (None, None)
    with _rest_pose(context, armature) as dg:
        obj_eval = obj.evaluated_get(dg)
        mesh = obj_eval.to_mesh()
        try:
            mesh.calc_loop_triangles()
            if not mesh.loop_triangles:
                return 'ERROR', f"{obj.name}: no faces to collide with"
            to_world = obj_eval.matrix_world
            normal_matrix = to_world.to_3x3().inverted_safe().transposed()
            deform = {b.name for b in armature.data.bones if b.use_deform} if armature else set()
            group_names = [g.name for g in obj_eval.vertex_groups]
            points, bindings, unweighted = [], [], 0
            for v in mesh.vertices:
                p = to_world @ v.co
                points.append((p, (normal_matrix @ v.normal).normalized()))
                if not armature:
                    bindings.append([])
                elif parent_bone:
                    bindings.append([(parent_bone, 1.0)])
                else:
                    top = _top_bones({group_names[g.group]: g.weight for g in v.groups
                                      if g.group < len(group_names) and group_names[g.group] in deform})
                    if not top:
                        unweighted += 1
                        top = [(_nearest_bone(armature, p), 1.0)]
                    bindings.append(top)
            triangles = [t.vertices[:] for t in mesh.loop_triangles]
        finally:
            obj_eval.to_mesh_clear()

    _write_tfxmesh(path, points, bindings, triangles)
    msg = f"{os.path.basename(path)}: {len(triangles)} triangles"
    if parent_bone:
        msg += f"; follows bone {parent_bone}"
    elif armature:
        msg += f"; skinned to {len({name for b in bindings for name, _ in b})} bones"
    if unweighted:
        return 'WARNING', f"{msg}; {unweighted} vertices without weights; they follow the nearest bone"
    return 'INFO', msg


def _collider_objects(context):
    return [o for o in context.view_layer.objects if o.type == 'MESH' and o.tressfx_collider]


def _collider_toggled(self, context):
    # A collider is a proxy: show it as a wireframe and keep it out of renders and the .glb.
    self.display_type = 'WIRE' if self.tressfx_collider else 'TEXTURED'
    self.hide_render = self.tressfx_collider


def _hair_objects(context):
    return [o for o in context.view_layer.objects if o.type == 'CURVES' and o.visible_get()]


def _hair_particle_systems(obj):
    if obj is None or obj.type != 'MESH':
        return []
    return [p for p in obj.particle_systems if p.settings.type == 'HAIR']


class EXPORT_OT_tressfx(bpy.types.Operator, ExportHelper):
    """Export the active hair object as TressFX guide strands (.tfx) and bone weights (.tfxbone)"""

    bl_idname = "export_scene.tressfx"
    bl_label = "Export TressFX Hair"

    filename_ext = ".tfx"
    filter_glob: StringProperty(default="*.tfx", options={'HIDDEN'})
    vertices_per_strand: EnumProperty(name="Vertices per Strand", description=VERTEX_COUNT_HELP,
                                      items=VERTEX_COUNTS, default="16")
    export_bones: BoolProperty(
        name="Bone Weights (.tfxbone)",
        description="Write a .tfxbone next to the .tfx when the surface mesh is moved by an armature",
        default=True,
    )

    def execute(self, context):
        obj = context.active_object
        if obj is None or obj.type != 'CURVES':
            if _hair_particle_systems(obj):
                self.report({'ERROR'}, "Particle hair: convert it first (Sidebar > TressFX > Convert Particle Hair)")
            else:
                self.report({'ERROR'}, "Select a hair object (Add > Curves > Empty Hair)")
            return {'CANCELLED'}
        level, msg = export_hair(context, obj, self.filepath, int(self.vertices_per_strand), self.export_bones)
        self.report({level}, "TressFX: " + msg)
        return {'CANCELLED'} if level == 'ERROR' else {'FINISHED'}


class TRESSFX_OT_add_hair(bpy.types.Operator):
    """Add an empty hair object on the active mesh and start sculpting it"""

    bl_idname = "tressfx.add_hair"
    bl_label = "Add Hair"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        obj = context.active_object
        return context.mode == 'OBJECT' and obj is not None and obj.type == 'MESH'

    def execute(self, context):
        mesh = context.active_object
        bpy.ops.object.curves_empty_hair_add()
        context.active_object.name = f"{mesh.name}_hair"
        bpy.ops.object.mode_set(mode='SCULPT_CURVES')
        try:
            bpy.ops.brush.asset_activate(
                asset_library_type='ESSENTIALS',
                relative_asset_identifier="brushes/essentials_brushes-curve_sculpt.blend/Brush/Add")
        except (AttributeError, RuntimeError, TypeError):
            pass  # ponytail: without brush assets (older Blender) the user picks the Add brush.
        self.report({'INFO'}, "Click on the head to plant hair")
        return {'FINISHED'}


class TRESSFX_OT_convert_particles(bpy.types.Operator):
    """Turn the mesh's particle hair into hair objects the exporter can read"""

    bl_idname = "tressfx.convert_particles"
    bl_label = "Convert Particle Hair"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return context.mode == 'OBJECT' and bool(_hair_particle_systems(context.active_object))

    def execute(self, context):
        mesh = context.active_object
        before = set(bpy.data.objects)
        bpy.ops.curves.convert_from_particle_system()
        uv = mesh.data.uv_layers.active
        for hair in [o for o in bpy.data.objects if o not in before and o.type == 'CURVES']:
            hair.name = f"{mesh.name}_hair"
            hair.data.surface = mesh
            hair.data.surface_uv_map = uv.name if uv else ""
            world = hair.matrix_world.copy()
            hair.parent = mesh
            hair.matrix_world = world
        # Hide the particle hair so it is not shown twice; delete it once the result looks right.
        for md in mesh.modifiers:
            if md.type == 'PARTICLE_SYSTEM' and md.particle_system.settings.type == 'HAIR':
                md.show_viewport = md.show_render = False
        return {'FINISHED'}


class TRESSFX_OT_export_all(bpy.types.Operator):
    """Export every visible hair object, and the character as .glb, into the export folder"""

    bl_idname = "tressfx.export_all"
    bl_label = "Export to Godot"

    def execute(self, context):
        settings = context.scene.tressfx
        folder = bpy.path.abspath(settings.folder)
        if not settings.folder or (settings.folder.startswith("//") and not bpy.data.filepath):
            self.report({'ERROR'}, "Pick an export folder first (or save the .blend file)")
            return {'CANCELLED'}
        os.makedirs(folder, exist_ok=True)
        if context.mode != 'OBJECT':
            bpy.ops.object.mode_set(mode='OBJECT')
        lines, worst = [], 'INFO'
        for obj in _hair_objects(context):
            path = os.path.join(folder, bpy.path.clean_name(obj.name) + ".tfx")
            level, msg = export_hair(context, obj, path, int(settings.vertices_per_strand), settings.export_bones)
            lines.append(msg if level == 'INFO' else "!" + msg)
            if level != 'INFO':
                worst = 'WARNING'  # One empty or unbound hair object does not stop the others.
        if settings.export_colliders:
            for obj in _collider_objects(context):
                path = os.path.join(folder, bpy.path.clean_name(obj.name) + ".tfxmesh")
                level, msg = export_collider(context, obj, path, settings.export_bones)
                lines.append(msg if level == 'INFO' else "!" + msg)
                if level != 'INFO':
                    worst = 'WARNING'
        if settings.export_character:
            name = settings.character_name or bpy.path.display_name_from_filepath(bpy.data.filepath) or "character"
            name = bpy.path.clean_name(name) + ".glb"
            # Modifiers applied: the hair was placed on the mesh as Blender shows it (subdivided...).
            # Render-disabled objects, the colliders among them, stay out.
            bpy.ops.export_scene.gltf(filepath=os.path.join(folder, name), export_format='GLB', export_yup=True,
                                      export_apply=True, use_renderable=True)
            lines.append(name)
        settings.last_report = " | ".join(lines)
        self.report({worst}, f"TressFX: {settings.last_report.replace('!', '')} -> {folder}")
        return {'FINISHED'}


class TressFXSettings(bpy.types.PropertyGroup):
    folder: StringProperty(name="Folder", subtype='DIR_PATH', default="//", options=PATH_OPTIONS,
                           description="Where the files are written, ideally a folder inside your Godot project")
    vertices_per_strand: EnumProperty(name="Vertices", description=VERTEX_COUNT_HELP, items=VERTEX_COUNTS,
                                      default="16")
    export_bones: BoolProperty(name="Bone Weights", default=True,
                               description="Write a .tfxbone so the hair follows the character's bones in Godot")
    export_colliders: BoolProperty(name="Colliders", default=True,
                                   description="Write every mesh marked as a hair collider as a .tfxmesh")
    export_character: BoolProperty(name="Character", default=True,
                                   description="Also export the whole scene as a .glb for Godot")
    character_name: StringProperty(name="File Name", description="Name of the .glb; the .blend name when empty")
    last_report: StringProperty()


def _draw_rig(layout, obj, rigid_text):
    armature, bone = _rig(obj)
    if bone:
        layout.label(text=f"Follows bone {bone}", icon='BONE_DATA')
    elif armature:
        layout.label(text=f"Skinned to {armature.name}", icon='ARMATURE_DATA')
    else:
        layout.label(text=rigid_text, icon='INFO')


class VIEW3D_PT_tressfx(bpy.types.Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "TressFX"
    bl_label = "TressFX Hair"

    def draw(self, context):
        layout = self.layout
        obj = context.active_object
        settings = context.scene.tressfx

        box = layout.box()
        box.label(text="1. Hair", icon='CURVES_DATA')
        if obj is not None and obj.type == 'CURVES':
            col = box.column()
            col.label(text=f"{obj.name}: {len(obj.data.curves)} curves")
            col.prop(obj.data, "surface", text="Surface")
            surface = obj.data.surface
            if surface is None:
                col.label(text="Pick the mesh the hair grows on", icon='ERROR')
            else:
                if surface.type == 'MESH':
                    col.prop_search(obj.data, "surface_uv_map", surface.data, "uv_layers", text="UV Map")
                _draw_rig(col, surface, "No armature: the hair stays rigid")
            row = box.row()
            row.scale_y = 1.3
            if obj.mode == 'SCULPT_CURVES':
                row.operator("object.mode_set", text="Done Sculpting", icon='OBJECT_DATAMODE').mode = 'OBJECT'
                tips = box.column(align=True)
                tips.scale_y = 0.8
                for tip in ("Add: click on the head to plant hair",
                            "Comb, Snake Hook: shape it",
                            "Grow / Shrink: length, Delete: remove",
                            "Collision: physics icon in the header"):
                    tips.label(text=tip)
            else:
                row.operator("object.mode_set", text="Sculpt Hair", icon='SCULPTMODE_HLT').mode = 'SCULPT_CURVES'
        elif obj is not None and obj.type == 'MESH' and not obj.tressfx_collider:
            if _hair_particle_systems(obj):
                box.operator(TRESSFX_OT_convert_particles.bl_idname, icon='PARTICLES')
            row = box.row()
            row.scale_y = 1.3
            row.operator(TRESSFX_OT_add_hair.bl_idname, text=f"Add Hair to {obj.name}", icon='ADD')
        else:
            box.label(text="Select the character's head", icon='INFO')

        box = layout.box()
        box.label(text="2. Colliders", icon='MOD_PHYSICS')
        col = box.column()
        if obj is not None and obj.type == 'MESH':
            col.prop(obj, "tressfx_collider", text=f"{obj.name} keeps hair out")
            if obj.tressfx_collider:
                _draw_rig(col, obj, "No armature: the collider stays rigid")
        else:
            tips = col.column(align=True)
            tips.scale_y = 0.8
            tips.label(text="Select a closed, low-poly mesh", icon='INFO')
            tips.label(text="to make it a hair collider", icon='BLANK1')

        box = layout.box()
        box.label(text="3. Export to Godot", icon='EXPORT')
        col = box.column()
        col.prop(settings, "folder")
        col.prop(settings, "vertices_per_strand")
        col.prop(settings, "export_bones")
        col.prop(settings, "export_colliders")
        row = col.row()
        row.prop(settings, "export_character")
        sub = row.row()
        sub.active = settings.export_character
        sub.prop(settings, "character_name", text="")
        hair = _hair_objects(context)
        files = box.column(align=True)
        files.scale_y = 0.8
        for h in hair:
            files.label(text=f"{bpy.path.clean_name(h.name)}.tfx  ({len(h.data.curves)} curves)", icon='CURVES_DATA')
        colliders = _collider_objects(context) if settings.export_colliders else []
        for c in colliders:
            files.label(text=f"{bpy.path.clean_name(c.name)}.tfxmesh", icon='MOD_PHYSICS')
        if not hair and not colliders:
            files.label(text="No hair objects yet", icon='INFO')
        row = box.row()
        row.scale_y = 1.5
        row.enabled = bool(hair or colliders)
        row.operator(TRESSFX_OT_export_all.bl_idname, icon='EXPORT')
        if settings.last_report:
            report = box.column(align=True)
            report.scale_y = 0.8
            for entry in settings.last_report.split(" | "):
                parts = entry.lstrip("!").split("; ")
                report.label(text=parts[0], icon='ERROR' if entry.startswith("!") else 'CHECKMARK')
                for part in parts[1:]:
                    report.label(text=part, icon='BLANK1')


CLASSES = (EXPORT_OT_tressfx, TRESSFX_OT_add_hair, TRESSFX_OT_convert_particles, TRESSFX_OT_export_all,
           TressFXSettings, VIEW3D_PT_tressfx)


def _menu(self, context):
    self.layout.operator(EXPORT_OT_tressfx.bl_idname, text="TressFX Hair (.tfx)")


def register():
    for cls in CLASSES:
        bpy.utils.register_class(cls)
    bpy.types.Scene.tressfx = PointerProperty(type=TressFXSettings)
    bpy.types.Object.tressfx_collider = BoolProperty(
        name="Hair Collider", update=_collider_toggled,
        description="Export this mesh as a .tfxmesh the hair stays out of, and leave it out of the .glb")
    bpy.types.TOPBAR_MT_file_export.append(_menu)


def unregister():
    bpy.types.TOPBAR_MT_file_export.remove(_menu)
    del bpy.types.Object.tressfx_collider
    del bpy.types.Scene.tressfx
    for cls in reversed(CLASSES):
        bpy.utils.unregister_class(cls)


if __name__ == "__main__":  # Run from Blender's Text Editor without installing.
    register()
