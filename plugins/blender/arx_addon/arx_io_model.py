# Copyright 2019-2021 Arx Libertatis Team (see the AUTHORS file)
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

import os
import itertools
import logging
import math

import bpy
import bmesh
from mathutils import Vector

from .arx_io_material import createMaterial
from .arx_io_util import (
    arx_pos_to_blender_for_model,
    blender_pos_to_arx,
    ArxException
)
from .files import splitPath
from .dataFtl import (
    FtlMetadata,
    FtlVertex,
    FtlFace,
    FtlGroup,
    FtlSelection,
    FtlAction,
    FtlData,
    FtlSerializer
)

class ArxObjectManager(object):
    def __init__(self, ioLib, dataPath):
        self.log = logging.getLogger('ArxObjectManager')
        self.ioLib = ioLib
        self.dataPath = dataPath
        self.ftlSerializer = FtlSerializer()

    def validateAssetDirectory(self):
        if not os.path.isdir(self.dataPath):
            raise ArxException("Arx assert directory path [" + self.dataPath + "] is not a directory")
        
        if not os.path.exists(self.dataPath):
            raise ArxException("Arx assert directory path [" + self.dataPath + "] does not exist")

    def validateObjectName(self, name):
        if len(name.encode('utf-8')) > 63:
            raise ArxException("Name ["+name+"] too long to be used as blender object name")

    def createBmesh(self, context, vertData, faceData, matCount) -> bmesh.types.BMesh:

        bm = bmesh.new()

        arxFaceType = bm.faces.layers.int.new('arx_facetype')
        arxTransVal = bm.faces.layers.float.new('arx_transval')
        # Original FTL vertex index + 1, so the export can merge copies again and keep the vertex order.
        # New vertices made in Blender get 0.
        arxVertex = bm.verts.layers.int.new('arx_vertex')

        uvData = bm.loops.layers.uv.verify()
        bmVerts = []
        for i, v in enumerate(vertData):
            vertex = bm.verts.new(arx_pos_to_blender_for_model(v.xyz))
            vertex[arxVertex] = i + 1
            bmVerts.append(vertex)

        for i, f in enumerate(faceData):

            faceVerts = [bmVerts[v] for v in f.vids]
            if len(set(faceVerts)) != len(faceVerts):
                self.log.warning("Skipping degenerate face %i" % i)
                continue

            if bm.faces.get(faceVerts) is not None:
                # Blender can not have two faces with the same vertices (back faces in the original models):
                # use copies of the vertices
                faceVerts = [bm.verts.new(v.co) for v in faceVerts]
                for j, v in enumerate(faceVerts):
                    v[arxVertex] = f.vids[j] + 1

            face = bm.faces.new(faceVerts)

            # Untextured faces use an empty material slot after the textures
            face.material_index = f.texid if f.texid >= 0 else matCount

            face[arxFaceType] = f.facetype
            face[arxTransVal] = f.transval

            for j, loop in enumerate(face.loops):
                u, v = f.uvs[j]
                loop[uvData].uv = u, 1.0 - v

        bm.faces.ensure_lookup_table()

        bm.edges.index_update()
        bm.edges.ensure_lookup_table()

        return bm

    def createObject(self, context, bm, data:FtlData, canonicalId, collection) -> bpy.types.Object:

        idString = "/".join(canonicalId);
        self.validateObjectName(idString)
        mesh = bpy.data.meshes.new(idString)
        bm.to_mesh(mesh)

        #armatureObj = self.createArmature(canonicalId, bm, data.groups)

        bm.free()

        if bpy.context.active_object:
            bpy.ops.object.mode_set(mode='OBJECT')

        obj = bpy.data.objects.new(name=mesh.name, object_data=mesh)
        #armatureObj.parent = obj # this created a dependency recursion, so i commented it

        obj['arx.ftl.name'] = data.metadata.name
        # Original FTL vertex indices (see the arx_vertex attribute)
        obj['arx.ftl.org'] = data.metadata.org
        obj['arx.ftl.group_origins'] = [g.origin for g in data.groups]

        # Blender vertices for each FTL vertex, more than one for copies made for back faces
        copies = [[] for _ in data.verts]
        for v in mesh.vertices:
            copies[mesh.attributes['arx_vertex'].data[v.index].value - 1].append(v.index)

        for i, g in enumerate(data.groups):
            grp = obj.vertex_groups.new(name="grp:" + str(i).zfill(2) + ":" + g.name)
            for v in g.indices:
                grp.add(copies[v], 1.0, 'ADD')

        for i, s in enumerate(data.sels):
            grp = obj.vertex_groups.new(name="sel:" + str(i).zfill(2) + ":" + s.name)

            for v in s.indices:
                grp.add(copies[v], 1.0, 'ADD')

        for a in data.actions:
            action = bpy.data.objects.new(a.name, None)
            action.parent = obj
            action.parent_type = 'VERTEX'
            action.parent_vertices = [a.vidx, 0, 0]  # last two are ignored
            action.show_name = True
            collection.objects.link(action)

            if a.name.lower().startswith("hit_"):
                radius = int(a.name[4:])
                action.empty_display_type = 'SPHERE'
                action.empty_display_size = radius
                action.lock_rotation = [True, True, True]
                action.lock_scale = [True, True, True]
            else:
                action.scale = [3, 3, 3]
                
            
        #armatureModifier = obj.modifiers.new(type='ARMATURE', name="Skeleton")
        #armatureModifier.object = armatureObj


        # for toDeSel in scene.objects: #deselect all first just to be sure
        #    toDeSel.select = False

        collection.objects.link(obj)
        #FIXME properly bind bones to mesh via vertex groups
        #obj.select = True
        #armatureObj.select = True
        #bpy.context.scene.objects.active = armatureObj #select both the mesh and armature and set armature active
        #bpy.ops.object.parent_set(type='ARMATURE_AUTO') # ctrl+p and automatic weights
        #bpy.context.scene.objects.active = obj
        #armatureObj.select = False

        #bpy.ops.object.mode_set(mode='EDIT', toggle=False)  # initialises UVmap correctly


        # TODO 2.8
        # mesh.uv_textures.new()
        for m in data.mats:
            mat = createMaterial(self.dataPath, m)
            obj.data.materials.append(mat)

        if any(f.texid < 0 for f in data.faces):
            obj.data.materials.append(None)

        #bpy.ops.object.mode_set(mode='OBJECT', toggle=False)
        return obj

    def createArmature(self, canonicalId, bm, groups) -> bpy.types.Object:

        amtname = "/".join(canonicalId)
        origin = (0, 0, 0)

        # Create armature and object
        amt = bpy.data.armatures.new(amtname)
        amt.draw_type = 'WIRE'
        amtobject = bpy.data.objects.new(amtname + "-amt", amt)
        # amtobject.show_x_ray = True
        amtobject.location = origin
        # ob.show_name = True

        # Link object to scene and make active
        bpy.context.scene.collection.link(amtobject)
        bpy.context.scene.collection.active = amtobject
        amtobject.select = True

        bpy.ops.object.mode_set(mode='EDIT')

        for i, (name, origin, indices, parentIndex) in enumerate(groups):
            bGrpName = "grp:" + str(i).zfill(2) + ":" + name

            bone = amt.edit_bones.new(bGrpName)
            bone.head = bm.verts[origin].co
            bone.tail = bm.verts[origin].co + Vector((0, -5, 0))
            bone["OriginVertex"] = origin

        editBonesArray = amt.edit_bones.values()
        for i, (name, origin, indices, parentIndex) in enumerate(groups):
            if parentIndex>=0:
                bone = editBonesArray[i]
                bone.parent = editBonesArray[parentIndex]

        bpy.ops.object.mode_set(mode='OBJECT')

        return amtobject

    def loadFile_data(self, filePath) -> FtlData:
        self.validateAssetDirectory();
        
        self.log.debug("Loading file: %s" % filePath)

        with open(filePath, "rb") as f:
            data = f.read()
            if data[:3] == b"FTL":
                unpacked = data
                self.log.debug("Loaded %i unpacked bytes from file %s" % (len(data), filePath))
            else:
                unpacked = self.ioLib.unpack(data)
                self.log.debug("Loaded %i packed bytes from file %s" % (len(data), filePath))

        ftlData = self.ftlSerializer.read(unpacked)
        return ftlData


    def loadFile(self, context, filePath, scene, import_tweaks: bool) -> bpy.types.Object:
        ftlData = self.loadFile_data(filePath)

        # create canonical id
        objPath = os.path.join(self.dataPath, "game/graph/obj3d/interactive")
        relPath = os.path.relpath(filePath, objPath)
        split = splitPath(relPath)
        canonicalId = split[:-1]

        # build the collection tree
        parent_collection = context.scene.collection
        idLen = len(canonicalId)
        for i in range(1, idLen + 1):
            prefix_str = "/".join(canonicalId[:i])
            if prefix_str in bpy.data.collections:
                parent_collection = bpy.data.collections[prefix_str]
            else:
                col = bpy.data.collections.new(prefix_str)
                parent_collection.children.link(col)
                parent_collection = col


        object_id_string = "/".join(canonicalId)

        self.log.debug("Canonical ID: %s" % object_id_string)

        #collection = bpy.data.collections.new(object_id_string)
        #context.scene.collection.children.link(collection)

        bm = self.createBmesh(context, ftlData.verts, ftlData.faces, len(ftlData.mats))
        obj = self.createObject(context, bm, ftlData, canonicalId, parent_collection)

        if import_tweaks:
            file_dir = os.path.dirname(filePath)
            tweak_dir = os.path.join(file_dir, 'tweaks')
            if os.path.isdir(tweak_dir):
                for rel_tweak_file in sorted(os.listdir(tweak_dir)):
                    if not rel_tweak_file.lower().endswith('.ftl'):
                        continue
                    abs_tweak_file = os.path.join(tweak_dir, rel_tweak_file)
                    tweak_data = self.loadFile_data(abs_tweak_file)
                    tweak_mesh = self.createBmesh(context, tweak_data.verts, tweak_data.faces, len(tweak_data.mats))

                    tweak_obj_id = canonicalId.copy()
                    tweak_obj_id.append(os.path.splitext(rel_tweak_file)[0])

                    tweak_obj = self.createObject(context, tweak_mesh, tweak_data, tweak_obj_id, parent_collection)
                    tweak_obj.parent = obj


        return obj

    # =============================================================

    def toFtlData(self, obj) -> FtlData:

        #objs = [o for o in bpy.data.objects if o.type == 'MESH' and not o.hide]

        #if not objs:
        #    self.log.info("Nothing to export")

        #obj = objs[0]

        canonicalId = obj.name.split("/")
        self.log.debug("Exporting Canonical Id: %s" % str(canonicalId))

        # obj.update_from_editmode()

        bm = bmesh.new()
        bm.from_mesh(obj.data)
        bm.normal_update()

        arxFaceType = bm.faces.layers.int.get('arx_facetype')
        if not arxFaceType:
            raise ArxException("Mesh is missing arx specific data layer: " + 'arx_facetype')
        
        arxTransVal = bm.faces.layers.float.get('arx_transval')
        if not arxTransVal:
            raise ArxException("Mesh is missing arx specific data layer: " + 'arx_transval')
        
        if not (math.isclose(obj.location[0], 0.0) and math.isclose(obj.location[1], 0.0) and math.isclose(obj.location[2], 0.0)):
            raise ArxException("Object is moved, please apply the location to the vertex positions")
        
        if not (math.isclose(obj.rotation_euler[0], 0.0) and math.isclose(obj.rotation_euler[1], 0.0) and math.isclose(obj.rotation_euler[2], 0.0)):
            raise ArxException("Object is rotated, please apply the rotation to the vertex positions")
        
        if not (math.isclose(obj.scale[0], 1.0) and math.isclose(obj.scale[1], 1.0) and math.isclose(obj.scale[2], 1.0)):
            raise ArxException("Object is scaled, please apply the scale to the vertex positions")
        

        for f in bm.faces:
            if len(f.loops) > 3:
                raise ArxException("Face with more than 3 vertices found")

        # Material slots → FTL textures, an empty slot means an untextured face
        matNames = []
        slotTexture = []
        for ms in obj.material_slots:
            mat = ms.material
            if mat is None:
                slotTexture.append(-1)
                continue
            texture = mat.get('arx.texture')
            if texture is None:
                if not mat.name.endswith('-mat'):
                    raise ArxException("Material names must end with -mat: " + mat.name)
                texture = 'GRAPH\\OBJ3D\\TEXTURES\\' + mat.name[:-4] + '.BMP'
            slotTexture.append(len(matNames))
            matNames.append(texture)

        # Vertices: copies of one original vertex (made on import for back faces) are merged again and the original
        # vertex order is kept. Vertices without an original index, or moved away from it, are appended.
        arxVertex = bm.verts.layers.int.get('arx_vertex')
        bm.verts.ensure_lookup_table()
        firstCopy = {}
        for v in bm.verts:
            key = v[arxVertex] if arxVertex else 0
            if key > 0 and key not in firstCopy:
                firstCopy[key] = v
        vertexKeys = []
        for v in bm.verts:
            key = v[arxVertex] if arxVertex else 0
            if key > 0 and firstCopy[key].co == v.co:
                vertexKeys.append(key)
            else:
                vertexKeys.append(-1 - v.index)
        order = sorted(set(k for k in vertexKeys if k > 0)) + [k for k in vertexKeys if k < 0]
        keyIndex = {k: i for i, k in enumerate(order)}
        vertexIndex = [keyIndex[k] for k in vertexKeys]
        originalIndex = {k - 1: i for k, i in keyIndex.items() if k > 0}

        verts = [None] * len(order)
        for v in bm.verts:
            i = vertexIndex[v.index]
            if verts[i] is None:
                verts[i] = FtlVertex(blender_pos_to_arx(v.co[:]), blender_pos_to_arx(v.normal[:]))

        org = originalIndex.get(obj.get('arx.ftl.org', -1))
        if org is None:
            for index, vert in enumerate(verts):
                if vert.xyz == (0.0, 0.0, 0.0):
                    org = index
        if org is None:
            self.log.info("Origin vertex not found, adding new one")
            org = len(verts)
            verts.append(FtlVertex((0.0, 0.0, 0.0), (0.0, 0.0, 0.0)))

        metadata = FtlMetadata(name=obj.get('arx.ftl.name', ''), org=org)

        # Action points: empties parented to the mesh
        actions = []
        for o in bpy.data.objects:
            if o.type == 'EMPTY' and o.parent == obj:
                name = o.name.split(".")[0]
                if o.parent_type == 'VERTEX':
                    if o.parent_vertices[0] >= len(bm.verts):
                        raise ArxException("Action point {} is parented to a deleted vertex".format(name))
                    actions.append(FtlAction(name, vertexIndex[o.parent_vertices[0]]))
                elif o.parent_type == 'OBJECT':
                    actions.append(FtlAction(name, len(verts)))
                    verts.append(FtlVertex(blender_pos_to_arx(o.location[:]), (0.0, 0.0, 0.0)))
                else:
                    self.log.warning("Unhandled empty parent type {}".format(o.parent_type))

        uvData = bm.loops.layers.uv.verify()

        faces = []
        for f in bm.faces:
            if f.material_index >= len(slotTexture):
                raise ArxException("Face uses a missing material slot")
            faces.append(FtlFace(
                vids=tuple(vertexIndex[c.vert.index] for c in f.loops),
                uvs=[(c[uvData].uv[0], 1 - c[uvData].uv[1]) for c in f.loops],
                texid=slotTexture[f.material_index],
                facetype=f[arxFaceType],
                transval=f[arxTransVal],
                normal=blender_pos_to_arx(f.normal[:])
            ))

        grps = []
        sels = []

        dvert_lay = bm.verts.layers.deform.active
        groupOrigins = obj.get('arx.ftl.group_origins')

        for grp in obj.vertex_groups:
            members = set()
            if dvert_lay is not None:
                for vert in bm.verts:
                    if grp.index in vert[dvert_lay]:
                        members.add(vertexIndex[vert.index])
            v = sorted(members)

            s = grp.name.split(":", 2)
            if s[0] == "grp":
                groupNumber = int(s[1])
                if groupOrigins is None or groupNumber >= len(groupOrigins):
                    raise ArxException("No origin (pivot) vertex for group " + grp.name)
                origin = originalIndex.get(groupOrigins[groupNumber])
                if origin is None:
                    raise ArxException("The origin (pivot) vertex of group {} was deleted".format(grp.name))
                # The parent is not stored, the game finds it from the group vertices
                grps.append((groupNumber, FtlGroup(s[2], origin, v, -1)))
            elif s[0] == "sel":
                sels.append((int(s[1]), FtlSelection(s[2], v)))

        # Animations address groups by index
        grps = [g for n, g in sorted(grps, key=lambda x: x[0])]
        sels = [s for n, s in sorted(sels, key=lambda x: x[0])]

        bm.free()

        return FtlData(
            metadata=metadata,
            verts=verts,
            faces=faces,
            mats=matNames,
            groups=grps,
            actions=actions,
            sels=sels
        )

    def saveFile(self, path):

        obj = bpy.context.active_object
        if not obj or obj.type != 'MESH':
            meshes = [o for o in bpy.context.selected_objects if o.type == 'MESH']
            if len(meshes) != 1:
                raise ArxException("Select the mesh object to export")
            obj = meshes[0]

        data = self.toFtlData(obj)

        binData = self.ftlSerializer.write(data)

        with open(path, 'wb') as f:
            f.write(binData)

        self.log.debug("Written %i bytes to file %s" % (len(binData), path))