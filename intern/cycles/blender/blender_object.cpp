/*
 * Copyright 2011-2013 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "render/camera.h"
#include "render/graph.h"
#include "render/integrator.h"
#include "render/light.h"
#include "render/mesh.h"
#include "render/nodes.h"
#include "render/object.h"
#include "render/particles.h"
#include "render/scene.h"
#include "render/shader.h"

#include "blender/blender_object_cull.h"
#include "blender/blender_sync.h"
#include "blender/blender_util.h"

#include "render/alembic.h"

#include "util/util_foreach.h"
#include "util/util_hash.h"
#include "util/util_logging.h"
#include "util/util_task.h"

CCL_NAMESPACE_BEGIN

/* Utilities */

bool BlenderSync::BKE_object_is_modified(BL::Object &b_ob)
{
  /* test if we can instance or if the object is modified */
  if (b_ob.type() == BL::Object::type_META) {
    /* multi-user and dupli metaballs are fused, can't instance */
    return true;
  }
  else if (ccl::BKE_object_is_modified(b_ob, b_scene, preview)) {
    /* modifiers */
    return true;
  }
  else {
    /* object level material links */
    BL::Object::material_slots_iterator slot;
    for (b_ob.material_slots.begin(slot); slot != b_ob.material_slots.end(); ++slot)
      if (slot->link() == BL::MaterialSlot::link_OBJECT)
        return true;
  }

  return false;
}

bool BlenderSync::object_is_geometry(BL::Object &b_ob)
{
  BL::ID b_ob_data = b_ob.data();

  if (!b_ob_data) {
    return false;
  }

  BL::Object::type_enum type = b_ob.type();

  if (type == BL::Object::type_VOLUME || type == BL::Object::type_HAIR) {
    /* Will be exported attached to mesh. */
    return true;
  }
  else if (type == BL::Object::type_CURVE) {
    /* Skip exporting curves without faces, overhead can be
     * significant if there are many for path animation. */
    BL::Curve b_curve(b_ob_data);

    return (b_curve.bevel_object() || b_curve.extrude() != 0.0f || b_curve.bevel_depth() != 0.0f ||
            b_curve.dimensions() == BL::Curve::dimensions_2D || b_ob.modifiers.length());
  }
  else {
    return (b_ob_data.is_a(&RNA_Mesh) || b_ob_data.is_a(&RNA_Curve) ||
            b_ob_data.is_a(&RNA_MetaBall));
  }
}

bool BlenderSync::object_is_light(BL::Object &b_ob)
{
  BL::ID b_ob_data = b_ob.data();

  return (b_ob_data && b_ob_data.is_a(&RNA_Light));
}

/* Object */

Object *BlenderSync::sync_object(BL::Depsgraph &b_depsgraph,
                                 BL::ViewLayer &b_view_layer,
                                 BL::DepsgraphObjectInstance &b_instance,
                                 float motion_time,
                                 bool use_particle_hair,
                                 bool show_lights,
                                 BlenderObjectCulling &culling,
                                 bool *use_portal,
                                 TaskPool *geom_task_pool)
{
  const bool is_instance = b_instance.is_instance();
  BL::Object b_ob = b_instance.object();
  BL::Object b_parent = is_instance ? b_instance.parent() : b_instance.object();
  BL::Object b_ob_instance = is_instance ? b_instance.instance_object() : b_ob;
  const bool motion = motion_time != 0.0f;
  /*const*/ Transform tfm = get_transform(b_ob.matrix_world());
  int *persistent_id = NULL;
  BL::Array<int, OBJECT_PERSISTENT_ID_SIZE> persistent_id_array;
  if (is_instance) {
    persistent_id_array = b_instance.persistent_id();
    persistent_id = persistent_id_array.data;
  }

  /* light is handled separately */
  if (!motion && object_is_light(b_ob)) {
    if (!show_lights) {
      return NULL;
    }

    /* TODO: don't use lights for excluded layers used as mask layer,
     * when dynamic overrides are back. */
#if 0
    if (!((layer_flag & view_layer.holdout_layer) && (layer_flag & view_layer.exclude_layer)))
#endif
    {
      sync_light(b_parent,
                 persistent_id,
                 b_ob,
                 b_ob_instance,
                 is_instance ? b_instance.random_id() : 0,
                 tfm,
                 use_portal);
    }

    return NULL;
  }

  /* only interested in object that we can create meshes from */
  if (!object_is_geometry(b_ob)) {
    return NULL;
  }

  /* Perform object culling. */
  if (culling.test(scene, b_ob, tfm)) {
    return NULL;
  }

  /* Visibility flags for both parent and child. */
  PointerRNA cobject = RNA_pointer_get(&b_ob.ptr, "cycles");
  bool use_holdout = get_boolean(cobject, "is_holdout") ||
                     b_parent.holdout_get(PointerRNA_NULL, b_view_layer);
  uint visibility = object_ray_visibility(b_ob) & PATH_RAY_ALL_VISIBILITY;

  if (b_parent.ptr.data != b_ob.ptr.data) {
    visibility &= object_ray_visibility(b_parent);
  }

  /* TODO: make holdout objects on excluded layer invisible for non-camera rays. */
#if 0
  if (use_holdout && (layer_flag & view_layer.exclude_layer)) {
    visibility &= ~(PATH_RAY_ALL_VISIBILITY - PATH_RAY_CAMERA);
  }
#endif

  /* Clear camera visibility for indirect only objects. */
  bool use_indirect_only = !use_holdout &&
                           b_parent.indirect_only_get(PointerRNA_NULL, b_view_layer);
  if (use_indirect_only) {
    visibility &= ~PATH_RAY_CAMERA;
  }

  /* Don't export completely invisible objects. */
  if (visibility == 0) {
    return NULL;
  }

  /* Use task pool only for non-instances, since sync_dupli_particle accesses
   * geometry. This restriction should be removed for better performance. */
  TaskPool *object_geom_task_pool = (is_instance) ? NULL : geom_task_pool;

  /* key to lookup object */
  ObjectKey key(b_parent, persistent_id, b_ob_instance, use_particle_hair);
  Object *object;

  /* motion vector case */
  if (motion) {
    object = object_map.find(key);

    if (object && object->use_motion()) {
      /* Set transform at matching motion time step. */
      int time_index = object->motion_step(motion_time);
      if (time_index >= 0) {
        array<Transform> motion = object->get_motion();
        motion[time_index] = tfm;
        object->set_motion(motion);
      }

      /* mesh deformation */
      if (object->get_geometry())
        sync_geometry_motion(b_depsgraph,
                             b_ob_instance,
                             object,
                             motion_time,
                             use_particle_hair,
							 object_geom_task_pool);
    }

    return object;
  }

  /* test if we need to sync */
  bool object_updated = false;

  if (object_map.add_or_update(&object, b_ob, b_parent, key))
    object_updated = true;

  /* mesh sync */
  /* b_ob is owned by the iterator and will go out of scope at the end of the block.
   * b_ob_instance is the original object and will remain valid for deferred geometry
   * sync. */
  Geometry *geometry = sync_geometry(b_depsgraph,
                                     b_ob_instance,
                                     b_ob_instance,
                                     object_updated,
                                     use_particle_hair,
                                     object_geom_task_pool);
  object->set_geometry(geometry);

  /* special case not tracked by object update flags */

  if (sync_object_attributes(b_instance, object)) {
    object_updated = true;
  }

  /* holdout */
  object->set_use_holdout(use_holdout);
  if (object->use_holdout_is_modified()) {
    scene->object_manager->tag_update(scene, HOLDOUT_MODIFIED);
  }

  object->set_visibility(visibility);

  bool is_shadow_catcher = get_boolean(cobject, "is_shadow_catcher");
  object->set_is_shadow_catcher(is_shadow_catcher);

  float shadow_terminator_offset = get_float(cobject, "shadow_terminator_offset");
  object->set_shadow_terminator_offset(shadow_terminator_offset);

  /* sync the asset name for Cryptomatte */
  BL::Object parent = b_ob.parent();
  ustring parent_name;
  if (parent) {
    while (parent.parent()) {
      parent = parent.parent();
    }
    parent_name = parent.name();
  }
  else {
    parent_name = b_ob.name();
  }
  object->set_asset_name(parent_name);

  /* object sync
   * transform comparison should not be needed, but duplis don't work perfect
   * in the depsgraph and may not signal changes, so this is a workaround */
  if (object->is_modified() || object_updated ||
      (object->get_geometry() && object->get_geometry()->is_modified()) ||
      tfm != object->get_tfm()) {
    object->name = b_ob.name().c_str();
    object->set_pass_id(b_ob.pass_index());
    object->set_color(get_float3(b_ob.color()));
    object->set_tfm(tfm);
    array<Transform> motion;
    object->set_motion(motion);

    /* motion blur */
    Scene::MotionType need_motion = scene->need_motion();
    if (need_motion != Scene::MOTION_NONE && object->get_geometry()) {
      Geometry *geom = object->get_geometry();
      geom->set_use_motion_blur(false);
      geom->set_motion_steps(0);

      uint motion_steps;

      if (need_motion == Scene::MOTION_BLUR) {
        motion_steps = object_motion_steps(b_parent, b_ob, Object::MAX_MOTION_STEPS);
        geom->set_motion_steps(motion_steps);
        if (motion_steps && object_use_deform_motion(b_parent, b_ob)) {
          geom->set_use_motion_blur(true);
        }
      }
      else {
        motion_steps = 3;
        geom->set_motion_steps(motion_steps);
      }

      motion.resize(motion_steps, transform_empty());

      if (motion_steps) {
        motion[motion_steps / 2] = tfm;

        /* update motion socket before trying to access object->motion_time */
        object->set_motion(motion);

        for (size_t step = 0; step < motion_steps; step++) {
          motion_times.insert(object->motion_time(step));
        }
      }
    }

    /* dupli texture coordinates and random_id */
    if (is_instance) {
      object->set_dupli_generated(0.5f * get_float3(b_instance.orco()) -
                                  make_float3(0.5f, 0.5f, 0.5f));
      object->set_dupli_uv(get_float2(b_instance.uv()));
      object->set_random_id(b_instance.random_id());
    }
    else {
      object->set_dupli_generated(make_float3(0.0f, 0.0f, 0.0f));
      object->set_dupli_uv(make_float2(0.0f, 0.0f));
      object->set_random_id(hash_uint2(hash_string(object->name.c_str()), 0));
    }

    object->tag_update(scene);
  }

  if (is_instance) {
    /* Sync possible particle data. */
    sync_dupli_particle(b_parent, b_instance, object);
  }

  return object;
}

/* This function mirrors drw_uniform_property_lookup in draw_instance_data.cpp */
static bool lookup_property(BL::ID b_id, const string &name, float4 *r_value)
{
  PointerRNA ptr;
  PropertyRNA *prop;

  if (!RNA_path_resolve(&b_id.ptr, name.c_str(), &ptr, &prop)) {
    return false;
  }

  PropertyType type = RNA_property_type(prop);
  int arraylen = RNA_property_array_length(&ptr, prop);

  if (arraylen == 0) {
    float value;

    if (type == PROP_FLOAT)
      value = RNA_property_float_get(&ptr, prop);
    else if (type == PROP_INT)
      value = RNA_property_int_get(&ptr, prop);
    else
      return false;

    *r_value = make_float4(value, value, value, 1.0f);
    return true;
  }
  else if (type == PROP_FLOAT && arraylen <= 4) {
    *r_value = make_float4(0.0f, 0.0f, 0.0f, 1.0f);
    RNA_property_float_get_array(&ptr, prop, &r_value->x);
    return true;
  }

  return false;
}

/* This function mirrors drw_uniform_attribute_lookup in draw_instance_data.cpp */
static float4 lookup_instance_property(BL::DepsgraphObjectInstance &b_instance,
                                       const string &name,
                                       bool use_instancer)
{
  string idprop_name = string_printf("[\"%s\"]", name.c_str());
  float4 value;

  /* If requesting instance data, check the parent particle system and object. */
  if (use_instancer && b_instance.is_instance()) {
    BL::ParticleSystem b_psys = b_instance.particle_system();

    if (b_psys) {
      if (lookup_property(b_psys.settings(), idprop_name, &value) ||
          lookup_property(b_psys.settings(), name, &value)) {
        return value;
      }
    }
    if (lookup_property(b_instance.parent(), idprop_name, &value) ||
        lookup_property(b_instance.parent(), name, &value)) {
      return value;
    }
  }

  /* Check the object and mesh. */
  BL::Object b_ob = b_instance.object();
  BL::ID b_data = b_ob.data();

  if (lookup_property(b_ob, idprop_name, &value) || lookup_property(b_ob, name, &value) ||
      lookup_property(b_data, idprop_name, &value) || lookup_property(b_data, name, &value)) {
    return value;
  }

  return make_float4(0.0f);
}

bool BlenderSync::sync_object_attributes(BL::DepsgraphObjectInstance &b_instance, Object *object)
{
  /* Find which attributes are needed. */
  AttributeRequestSet requests = object->get_geometry()->needed_attributes();

  /* Delete attributes that became unnecessary. */
  vector<ParamValue> &attributes = object->attributes;
  bool changed = false;

  for (int i = attributes.size() - 1; i >= 0; i--) {
    if (!requests.find(attributes[i].name())) {
      attributes.erase(attributes.begin() + i);
      changed = true;
    }
  }

  /* Update attribute values. */
  foreach (AttributeRequest &req, requests.requests) {
    ustring name = req.name;

    std::string real_name;
    BlenderAttributeType type = blender_attribute_name_split_type(name, &real_name);

    if (type != BL::ShaderNodeAttribute::attribute_type_GEOMETRY) {
      bool use_instancer = (type == BL::ShaderNodeAttribute::attribute_type_INSTANCER);
      float4 value = lookup_instance_property(b_instance, real_name, use_instancer);

      /* Try finding the existing attribute value. */
      ParamValue *param = NULL;

      for (size_t i = 0; i < attributes.size(); i++) {
        if (attributes[i].name() == name) {
          param = &attributes[i];
          break;
        }
      }

      /* Replace or add the value. */
      ParamValue new_param(name, TypeDesc::TypeFloat4, 1, &value);
      assert(new_param.datasize() == sizeof(value));

      if (!param) {
        changed = true;
        attributes.push_back(new_param);
      }
      else if (memcmp(param->data(), &value, sizeof(value)) != 0) {
        changed = true;
        *param = new_param;
      }
    }
  }

  return changed;
}

/* Object Loop */

static BL::MeshSequenceCacheModifier object_alembic_cache_find(BL::Object b_ob)
{
  if (b_ob.modifiers.length() > 0) {
    BL::Modifier b_mod = b_ob.modifiers[b_ob.modifiers.length() - 1];

    if (b_mod.type() == BL::Modifier::type_MESH_SEQUENCE_CACHE) {
      return BL::MeshSequenceCacheModifier(b_mod);
    }
  }

  return BL::MeshSequenceCacheModifier(PointerRNA_NULL);
}

void BlenderSync::sync_procedural(BL::Object &b_ob,
                                  BL::MeshSequenceCacheModifier &b_mesh_cache,
                                  int frame_current,
                                  float motion_time)
{
  bool motion = motion_time != 0.0f;

  if (motion) {
    return;
  }

  BL::CacheFile cache_file = b_mesh_cache.cache_file();
  void *cache_file_key = cache_file.ptr.data;

  AlembicProcedural *p = static_cast<AlembicProcedural *>(procedural_map.find(cache_file_key));

  if (!p) {
    p = scene->create_node<AlembicProcedural>();
    procedural_map.add(cache_file_key, p);
  }
  else {
    procedural_map.used(p);
  }

  p->set_frame(static_cast<float>(frame_current));
  if (p->frame_is_modified()) {
    scene->procedural_manager->need_update = true;
  }

  auto absolute_path = blender_absolute_path(b_data, b_ob, b_mesh_cache.cache_file().filepath());

  p->set_filepath(ustring(absolute_path));

  /* if the filepath was not modified, then we have already created the objects */
  if (!p->filepath_is_modified()) {
    return;
  }

  Shader *default_shader = (b_ob.type() == BL::Object::type_VOLUME) ? scene->default_volume :
                                                                      scene->default_surface;
  /* Find shader indices. */
  array<Node *> used_shaders;

  BL::Object::material_slots_iterator slot;
  for (b_ob.material_slots.begin(slot); slot != b_ob.material_slots.end(); ++slot) {
    BL::ID b_material(slot->material());
    find_shader(b_material, used_shaders, default_shader);
  }

  if (used_shaders.size() == 0) {
    used_shaders.push_back_slow(default_shader);
  }

  AlembicObject *abc_object = scene->create_node<AlembicObject>();
  abc_object->set_path(ustring(b_mesh_cache.object_path()));
  abc_object->set_used_shaders(used_shaders);

  p->objects.push_back_slow(abc_object);
}

void BlenderSync::sync_objects(BL::Depsgraph &b_depsgraph,
                               BL::SpaceView3D &b_v3d,
                               float motion_time)
{
  /* Task pool for multithreaded geometry sync. */
  TaskPool geom_task_pool;

  /* layer data */
  bool motion = motion_time != 0.0f;

  if (!motion) {
    /* prepare for sync */
    light_map.pre_sync();
    geometry_map.pre_sync();
    object_map.pre_sync();
    procedural_map.pre_sync();
    particle_system_map.pre_sync();
    motion_times.clear();
  }
  else {
    geometry_motion_synced.clear();
  }

  /* initialize culling */
  BlenderObjectCulling culling(scene, b_scene);

  /* object loop */
  bool cancel = false;
  bool use_portal = false;
  const bool show_lights = BlenderViewportParameters(b_v3d).use_scene_lights;

  BL::ViewLayer b_view_layer = b_depsgraph.view_layer_eval();
  BL::Depsgraph::object_instances_iterator b_instance_iter;

  for (b_depsgraph.object_instances.begin(b_instance_iter);
       b_instance_iter != b_depsgraph.object_instances.end() && !cancel;
       ++b_instance_iter) {
    BL::DepsgraphObjectInstance b_instance = *b_instance_iter;
    BL::Object b_ob = b_instance.object();

    /* Viewport visibility. */
    const bool show_in_viewport = !b_v3d || b_ob.visible_in_viewport_get(b_v3d);
    if (show_in_viewport == false) {
      continue;
    }

    /* Load per-object culling data. */
    culling.init_object(scene, b_ob);

    /* Ensure the object geom supporting the hair is processed before adding
     * the hair processing task to the task pool, calling .to_mesh() on the
     * same object in parallel does not work. */
    const bool sync_hair = b_instance.show_particles() && object_has_particle_hair(b_ob);

    /* Object itself. */
    if (b_instance.show_self()) {
      BL::MeshSequenceCacheModifier b_mesh_cache = object_alembic_cache_find(b_ob);

      if (b_mesh_cache) {
        sync_procedural(b_ob, b_mesh_cache, b_depsgraph.scene().frame_current(), motion_time);
      }
      else {
        sync_object(b_depsgraph,
                    b_view_layer,
                    b_instance,
                    motion_time,
                    false,
                    show_lights,
                    culling,
                    &use_portal,
                    sync_hair ? NULL : &geom_task_pool);
      }
    }

    /* Particle hair as separate object. */
    if (sync_hair) {
      sync_object(b_depsgraph,
                  b_view_layer,
                  b_instance,
                  motion_time,
                  true,
                  show_lights,
                  culling,
                  &use_portal,
                  &geom_task_pool);
    }

    cancel = progress.get_cancel();
  }

  geom_task_pool.wait_work();

  progress.set_sync_status("");

  if (!cancel && !motion) {
    sync_background_light(b_v3d, use_portal);

    /* handle removed data and modified pointers */
    light_map.post_sync();
    geometry_map.post_sync();
    object_map.post_sync();
    particle_system_map.post_sync();
    procedural_map.post_sync();
  }

  if (motion)
    geometry_motion_synced.clear();
}

void BlenderSync::sync_motion(BL::RenderSettings &b_render,
                              BL::Depsgraph &b_depsgraph,
                              BL::SpaceView3D &b_v3d,
                              BL::Object &b_override,
                              int width,
                              int height,
                              void **python_thread_state)
{
  if (scene->need_motion() == Scene::MOTION_NONE)
    return;

  /* get camera object here to deal with camera switch */
  BL::Object b_cam = b_scene.camera();
  if (b_override)
    b_cam = b_override;

  int frame_center = b_scene.frame_current();
  float subframe_center = b_scene.frame_subframe();
  float frame_center_delta = 0.0f;

  if (scene->need_motion() != Scene::MOTION_PASS &&
      scene->camera->get_motion_position() != Camera::MOTION_POSITION_CENTER) {
    float shuttertime = scene->camera->get_shuttertime();
    if (scene->camera->get_motion_position() == Camera::MOTION_POSITION_END) {
      frame_center_delta = -shuttertime * 0.5f;
    }
    else {
      assert(scene->camera->get_motion_position() == Camera::MOTION_POSITION_START);
      frame_center_delta = shuttertime * 0.5f;
    }

    float time = frame_center + subframe_center + frame_center_delta;
    int frame = (int)floorf(time);
    float subframe = time - frame;
    python_thread_state_restore(python_thread_state);
    b_engine.frame_set(frame, subframe);
    python_thread_state_save(python_thread_state);
    if (b_cam) {
      sync_camera_motion(b_render, b_cam, width, height, 0.0f);
    }
    sync_objects(b_depsgraph, b_v3d, 0.0f);
  }

  /* Insert motion times from camera. Motion times from other objects
   * have already been added in a sync_objects call. */
  if (b_cam) {
    uint camera_motion_steps = object_motion_steps(b_cam, b_cam);
    for (size_t step = 0; step < camera_motion_steps; step++) {
      motion_times.insert(scene->camera->motion_time(step));
    }
  }

  /* note iteration over motion_times set happens in sorted order */
  foreach (float relative_time, motion_times) {
    /* center time is already handled. */
    if (relative_time == 0.0f) {
      continue;
    }

    VLOG(1) << "Synchronizing motion for the relative time " << relative_time << ".";

    /* fixed shutter time to get previous and next frame for motion pass */
    float shuttertime = scene->motion_shutter_time();

    /* compute frame and subframe time */
    float time = frame_center + subframe_center + frame_center_delta +
                 relative_time * shuttertime * 0.5f;
    int frame = (int)floorf(time);
    float subframe = time - frame;

    /* change frame */
    python_thread_state_restore(python_thread_state);
    b_engine.frame_set(frame, subframe);
    python_thread_state_save(python_thread_state);

    /* Syncs camera motion if relative_time is one of the camera's motion times. */
    sync_camera_motion(b_render, b_cam, width, height, relative_time);

    /* sync object */
    sync_objects(b_depsgraph, b_v3d, relative_time);
  }

  /* we need to set the python thread state again because this
   * function assumes it is being executed from python and will
   * try to save the thread state */
  python_thread_state_restore(python_thread_state);
  b_engine.frame_set(frame_center, subframe_center);
  python_thread_state_save(python_thread_state);
}

CCL_NAMESPACE_END
