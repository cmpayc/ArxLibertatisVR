# Copyright 2026 Arx Libertatis VR contributors <kazakov.s.fl@gmail.com>
#
# This file is part of Arx Libertatis.
#
# Arx Libertatis is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# Arx Libertatis is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with Arx Libertatis. If not, see <http://www.gnu.org/licenses/>.

"""
Transfer the Arx rig (bone groups, selections, pivots, action points) from an imported FTL model to a new mesh.

The new mesh can be anything placed over the original in its rest pose: a subdivided or sculpted copy, or a new model.
Each new vertex gets the bone group and selections of the nearest original vertex. The group lists are nested like in
the original files (a group also contains the vertices of its child groups). The pivot and action point vertices of the
original are copied as loose vertices, so the bone positions stay exactly the same.
"""

import logging

import bpy
import bmesh
from mathutils.bvhtree import BVHTree

from .arx_io_util import ArxException

log = logging.getLogger('ArxRig')


def parseGroupName(name):
    parts = name.split(":", 2)
    if len(parts) != 3 or parts[0] not in ("grp", "sel"):
        return None
    return parts[0], int(parts[1]), parts[2]


def groupParents(groups, origins):
    """Parent of each group as the game computes it: the nearest earlier group that contains the group's pivot vertex"""
    parents = []
    for i in range(len(groups)):
        parent = None
        for j in range(i - 1, -1, -1):
            if origins[i] in groups[j]:
                parent = j
                break
        parents.append(parent)
    return parents


def transferRig(source, target):

    origins = source.get('arx.ftl.group_origins')
    if origins is None or 'arx_vertex' not in source.data.attributes:
        raise ArxException("The source must be a model imported from an FTL file: " + source.name)
    if source.matrix_world != target.matrix_world:
        raise ArxException("Source and target must have the same location, rotation and scale")

    src = bmesh.new()
    src.from_mesh(source.data)
    src.verts.ensure_lookup_table()
    src.faces.ensure_lookup_table()
    srcArxVertex = src.verts.layers.int.get('arx_vertex')
    srcDeform = src.verts.layers.deform.active
    srcFaceType = src.faces.layers.int.get('arx_facetype')
    srcTransVal = src.faces.layers.float.get('arx_transval')

    # Source groups and selections, indexed by their FTL number
    groupNames = {}
    selectionNames = {}
    vgroupToGroup = {}
    vgroupToSelection = {}
    for vg in source.vertex_groups:
        parsed = parseGroupName(vg.name)
        if parsed is None:
            continue
        kind, number, name = parsed
        if kind == "grp":
            groupNames[number] = vg.name
            vgroupToGroup[vg.index] = number
        else:
            selectionNames[number] = vg.name
            vgroupToSelection[vg.index] = number
    groupCount = len(groupNames)
    if sorted(groupNames) != list(range(groupCount)) or len(origins) != groupCount:
        raise ArxException("The source bone groups are not numbered 0.." + str(groupCount - 1))

    # Membership per source vertex: all groups (nested lists as in the file), innermost group, selections
    srcGroups = []
    srcInner = []
    srcSelections = []
    groupMembers = [set() for _ in range(groupCount)]
    ftlToSource = {}
    for v in src.verts:
        ftlToSource.setdefault(v[srcArxVertex] - 1, v.index)
        dv = v[srcDeform] if srcDeform else {}
        groups = sorted(vgroupToGroup[i] for i in dv.keys() if i in vgroupToGroup)
        srcGroups.append(groups)
        srcInner.append(groups[-1] if groups else None)
        srcSelections.append([vgroupToSelection[i] for i in dv.keys() if i in vgroupToSelection])
        for g in groups:
            groupMembers[g].add(v[srcArxVertex] - 1)

    parents = groupParents(groupMembers, list(origins))
    def chain(group):
        out = []
        while group is not None:
            out.append(group)
            group = parents[group]
        return out

    # Vertices that must stay at their exact position: pivots, the model origin and action points
    keep = set(origins) | {source.get('arx.ftl.org', -1)}
    actions = []
    for child in source.children:
        if child.type == 'EMPTY' and child.parent_type == 'VERTEX':
            actions.append(child)
            keep.add(src.verts[child.parent_vertices[0]][srcArxVertex] - 1)
    for v in src.verts:
        if not v.link_faces:
            keep.add(v[srcArxVertex] - 1)
    keep.discard(-1)
    keep.discard(-2)

    tree = BVHTree.FromBMesh(src)

    dst = bmesh.new()
    dst.from_mesh(target.data)

    # Loose vertices of the target (for example old pivots in a copy of the source) are replaced by the source ones
    for v in [v for v in dst.verts if not v.link_faces]:
        dst.verts.remove(v)
    bmesh.ops.triangulate(dst, faces=dst.faces[:])
    dst.verts.ensure_lookup_table()

    dstArxVertex = dst.verts.layers.int.get('arx_vertex') or dst.verts.layers.int.new('arx_vertex')
    dstFaceType = dst.faces.layers.int.get('arx_facetype')
    dstTransVal = dst.faces.layers.float.get('arx_transval')
    copyFaceData = dstFaceType is None or dstTransVal is None
    if dstFaceType is None:
        dstFaceType = dst.faces.layers.int.new('arx_facetype')
    if dstTransVal is None:
        dstTransVal = dst.faces.layers.float.new('arx_transval')
    dstDeform = dst.verts.layers.deform.verify()

    # The target vertex groups mirror the source ones, in the same order
    target.vertex_groups.clear()
    for vg in source.vertex_groups:
        target.vertex_groups.new(name=vg.name)
    groupIndex = {g: target.vertex_groups[groupNames[g]].index for g in groupNames}
    selectionIndex = {s: target.vertex_groups[selectionNames[s]].index for s in selectionNames}

    def nearestSourceVertex(co):
        location, normal, faceIndex, distance = tree.find_nearest(co)
        if faceIndex is None:
            return None, None
        face = src.faces[faceIndex]
        corner = min(face.verts, key=lambda v: (v.co - location).length_squared)
        return corner.index, face

    unassigned = 0
    for v in dst.verts:
        v[dstArxVertex] = 0  # new vertex
        dv = v[dstDeform]
        dv.clear()
        sourceIndex, face = nearestSourceVertex(v.co)
        if sourceIndex is None or srcInner[sourceIndex] is None:
            unassigned += 1
            continue
        for g in chain(srcInner[sourceIndex]):
            dv[groupIndex[g]] = 1.0
        for s in srcSelections[sourceIndex]:
            dv[selectionIndex[s]] = 1.0

    if copyFaceData:
        for f in dst.faces:
            sourceIndex, face = nearestSourceVertex(f.calc_center_median())
            if face is not None:
                f[dstFaceType] = face[srcFaceType] if srcFaceType else 0
                f[dstTransVal] = face[srcTransVal] if srcTransVal else 0.0

    # Copy pivots, the origin and action points as loose vertices with their original FTL index and membership
    copied = {}
    for ftlIndex in sorted(keep):
        sv = src.verts[ftlToSource[ftlIndex]]
        v = dst.verts.new(sv.co)
        v[dstArxVertex] = ftlIndex + 1
        dv = v[dstDeform]
        for g in srcGroups[sv.index]:
            dv[groupIndex[g]] = 1.0
        for s in srcSelections[sv.index]:
            dv[selectionIndex[s]] = 1.0
        copied[ftlIndex] = v

    dst.verts.index_update()
    actionVertices = [(a, copied[src.verts[a.parent_vertices[0]][srcArxVertex] - 1].index) for a in actions]

    dst.to_mesh(target.data)
    dst.free()
    src.free()

    target['arx.ftl.name'] = source.get('arx.ftl.name', '')
    target['arx.ftl.org'] = source.get('arx.ftl.org', -1)
    target['arx.ftl.group_origins'] = list(origins)

    # Action points
    for child in list(target.children):
        if child.type == 'EMPTY':
            bpy.data.objects.remove(child)
    for action, vertexIndex in actionVertices:
        empty = bpy.data.objects.new(action.name.split(".")[0], None)
        empty.parent = target
        empty.parent_type = 'VERTEX'
        empty.parent_vertices = [vertexIndex, 0, 0]
        empty.show_name = True
        empty.empty_display_type = action.empty_display_type
        empty.empty_display_size = action.empty_display_size
        empty.scale = action.scale
        for collection in target.users_collection:
            collection.objects.link(empty)

    if unassigned:
        log.warning("%i vertices are not near the source surface and have no bone group" % unassigned)
    return unassigned


class ArxOperatorTransferRig(bpy.types.Operator):
    """Copy the bone groups, selections, pivots and action points of the active imported model to the selected meshes"""
    bl_idname = "arx.transfer_rig"
    bl_label = "Transfer Arx Rig"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return context.mode == 'OBJECT' and context.active_object is not None \
            and context.active_object.type == 'MESH' and len(context.selected_objects) >= 2

    def execute(self, context):
        source = context.active_object
        targets = [o for o in context.selected_objects if o != source and o.type == 'MESH']
        try:
            for target in targets:
                unassigned = transferRig(source, target)
                if unassigned:
                    self.report({'WARNING'}, "{}: {} vertices have no bone group".format(target.name, unassigned))
        except ArxException as e:
            self.report({'ERROR'}, str(e))
            return {'CANCELLED'}
        self.report({'INFO'}, "Transferred the rig of {} to {} mesh(es)".format(source.name, len(targets)))
        return {'FINISHED'}


class ArxRigPanel(bpy.types.Panel):
    bl_idname = "VIEW3D_PT_arx_rig"
    bl_label = "Arx Rig"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "Arx"

    def draw(self, context):
        layout = self.layout
        layout.label(text="Select new mesh(es), then the imported model")
        layout.operator(ArxOperatorTransferRig.bl_idname)
