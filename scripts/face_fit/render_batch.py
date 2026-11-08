# blender -b -P render_batch.py -- list.txt   (each line: glb png)
import bpy, sys, mathutils
lst=[l.split() for l in open(sys.argv[sys.argv.index('--')+1]) if l.strip()]
for glb,out in lst:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=glb)
    m=[o for o in bpy.data.objects if o.type=='MESH' and o.data.shape_keys][0]
    dg=bpy.context.evaluated_depsgraph_get(); me=m.evaluated_get(dg).to_mesh()
    vs=[m.matrix_world@v.co for v in me.vertices]; ztop=max(v.z for v in vs)
    c=mathutils.Vector((0,0.1,ztop-0.125))
    cam=bpy.data.objects.new('cam',bpy.data.cameras.new('cam')); bpy.context.collection.objects.link(cam); bpy.context.scene.camera=cam
    cam.data.lens=85; cam.location=c+mathutils.Vector((0,0.65,0)); cam.rotation_euler=(c-cam.location).to_track_quat('-Z','Y').to_euler()
    for loc,en in (((1,-1,2),3.0),((-1,-0.5,2),1.5),((0,-1,0.5),1.0)):
        l=bpy.data.lights.new('l','SUN'); l.energy=en; lo=bpy.data.objects.new('l',l); lo.rotation_euler=mathutils.Vector(loc).to_track_quat('-Z','Y').to_euler(); bpy.context.collection.objects.link(lo)
    bpy.context.scene.world=bpy.data.worlds.new('w'); bpy.context.scene.world.color=(0.35,0.35,0.35)
    r=bpy.context.scene.render; r.engine='BLENDER_WORKBENCH'; r.resolution_x=512; r.resolution_y=512; r.filepath=out
    bpy.context.scene.display.shading.light='STUDIO'; bpy.context.scene.display.shading.color_type='TEXTURE'
    bpy.ops.render.render(write_still=True)
