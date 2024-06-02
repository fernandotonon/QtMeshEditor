/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------------
*/

#include "MaterialHighlighter.h"
#include <stdio.h>
#include <QRegularExpression>
#include <QApplication>
#include <QPalette>

MaterialHighlighter::MaterialHighlighter(QObject *parent) :
    QSyntaxHighlighter(parent)
{
    mParent = parent;
}

MaterialHighlighter::~MaterialHighlighter()
{
}

void MaterialHighlighter::highlightBlock(const QString &text)
{
    if(mParent) mParent->blockSignals(true);

    QColor palette = QApplication::palette().color(QPalette::Text);
    bool dark = qGray(palette.red(),palette.green(),palette.blue())>100;

    QTextCharFormat format;
    format.setFontWeight(QFont::Bold);
    format.setForeground(dark?QColor("mediumorchid"):Qt::darkBlue);
    QString pattern = "\\b(compositor|target|target_output|compositor_logic|scheme|vertex_program|geometry_program|fragment_program|default_params|material|technique|pass|texture_unit|vertex_program_ref|geometry_program_ref|fragment_program_ref|shadow_caster_vertex_program_ref|shadow_receiver_fragment_program_ref|shadow_receiver_vertex_program_ref|abstract|import|from|cg|hlsl|glsl)\\b";
    applyHighlight(format,pattern, text);

    format.setFontWeight(QFont::Bold);
    format.setForeground(dark?QColor("limegreen"):Qt::darkRed);
    pattern = "\\b(input|texture|render_quad|source|syntax|manual_named_constants|entry_point|profiles|includes_skeletal_animation|includes_morph_animation|includes_pose_animation|uses_vertex_texture_fetch|uses_adjacency_information|target|preprocessor_defines|column_major_matrices|attach|input_operation_type|output_operation_type|max_output_vertices|delegate|param_indexed|param_indexed_auto|param_named|param_named_auto|lod_distances|receive_shadows|transparency_casts_shadows|set|set_texture_alias|scheme|lod_index|shadow_caster_material|shadow_receiver_material|gpu_vendor_rule|gpu_device_rule|ambient|diffuse|specular|emissive|scene_blend|separate_scene_blend|depth_check|depth_write|depth_func|depth_bias|iteration_depth_bias|alpha_rejection|alpha_to_coverage|light_scissor|light_clip_planes|illumination_stage|transparent_sorting|normalise_normals|cull_hardware|cull_software|lighting|shading|polygon_mode|polygon_mode_overrideable|fog_override|colour_write|max_lights|start_light|iteration|point_size|point_sprites|point_size_attenuation|point_size_min|point_size_max|texture_alias|texture|anim_texture|cubic_texture|tex_coord_set|tex_address_mode|tex_border_colour|filtering|max_anisotropy|mipmap_bias|colour_op|colour_op_ex|colour_op_multipass_fallback|alpha_op_ex|env_map|scroll|scroll_anim|rotate|rotate_anim|scale|wave_xform|transform|binding_type|content_type)\\b";
    applyHighlight(format,pattern, text);

    format.setFontWeight(QFont::Normal);
    format.setForeground(dark?QColor("mediumslateblue"):Qt::darkCyan);
    pattern = "\\b(PF_A8R8G8B8|PF_R8G8B8A8|PF_R8G8B8|PF_FLOAT16_RGBA|PF_FLOAT16_RGB|PF_FLOAT16_R|PF_FLOAT32_RGBA|PF_FLOAT32_RGB|local_scope|chain_scope|global_scope|PF_FLOAT32_R|int|half|float|float2|float3|float4|float3x3|float3x4|float4x3|float4x4|double|include|exclude|true|false|on|off|none|vertexcolour|add|modulate|alpha_blend|colour_blend|one|zero|dest_colour|src_colour|one_minus_dest_colour|one_minus_src_colour|dest_alpha|src_alpha|one_minus_dest_alpha|one_minus_src_alpha|always_fail|always_pass|less|less_equal|equal|not_equal|greater_equal|greater|clockwise|anticlockwise|back|front|flat|gouraud|phong|solid|wireframe|points|type|linear|exp|exp2|colour|density|start|end|once|once_per_light|per_light|per_n_lights|point|directional|spot|1d|2d|3d|cubic|PF_L8|PF_L16|PF_A8|PF_A4L4|PF_BYTE_LA|PF_R5G6B5|PF_B5G6R5|PF_R3G3B2|PF_A4R4G4B4|PF_A1R5G5B5|PF_R8G8B8|PF_B8G8R8|PF_A8R8G8B8|PF_A8B8G8R8|PF_B8G8R8A8|PF_R8G8B8A8|PF_X8R8G8B8|PF_X8B8G8R8|PF_A2R10G10B10|PF_A2B10G10R10|PF_FLOAT16_R|PF_FLOAT16_RGB|PF_FLOAT16_RGBA|PF_FLOAT32_R|PF_FLOAT32_RGB|PF_FLOAT32_RGBA|PF_SHORT_RGBA|combinedUVW|separateUV|vertex|fragment|named|shadow|wrap|clamp|mirror|border|bilinear|trilinear|anisotropic|replace|source1|source2|modulate_x2|modulate_x4|add_signed|add_smooth|subtract|blend_diffuse_alpha|blend_texture_alpha|blend_current_alpha|blend_manual|dotproduct|blend_diffuse_colour|src_current|src_texture|src_diffuse|src_specular|src_manual|spherical|planar|cubic_reflection|cubic_normal|xform_type|scroll_x|scroll_y|scale_x|scale_y|wave_type|sine|triangle|square|sawtooth|inverse_sawtooth|base|frequency|phase|amplitude|arbfp1|arbvp1|glslv|glslf|gp4vp|gp4gp|gp4fp|fp20|fp30|fp40|vp20|vp30|vp40|ps_1_1|ps_1_2|ps_1_3|ps_1_4|ps_2_0|ps_2_a|ps_2_b|ps_2_x|ps_3_0|ps_3_x|vs_1_1|vs_2_0|vs_2_a|vs_2_x|vs_3_0|1d|2d|3d|4d)\\b";
    applyHighlight(format,pattern, text);

    format.setFontWeight(QFont::Normal);
    format.setForeground(dark?QColor("mediumvioletred"):Qt::darkMagenta);
    pattern = "\\b(gamma|pooled|no_fsaa|depth_pool|scope|target_width|target_height|target_width_scaled|target_height|scaled|previous|world_matrix|inverse_world_matrix|transpose_world_matrix|inverse_transpose_world_matrix|world_matrix_array_3x4|view_matrix|inverse_view_matrix|transpose_view_matrix|inverse_transpose_view_matrix|projection_matrix|inverse_projection_matrix|transpose_projection_matrix|inverse_transpose_projection_matrix|worldview_matrix|inverse_worldview_matrix|transpose_worldview_matrix|inverse_transpose_worldview_matrix|viewproj_matrix|inverse_viewproj_matrix|transpose_viewproj_matrix|inverse_transpose_viewproj_matrix|worldviewproj_matrix|inverse_worldviewproj_matrix|transpose_worldviewproj_matrix|inverse_transpose_worldviewproj_matrix|texture_matrix|render_target_flipping|light_diffuse_colour|light_specular_colour|light_attenuation|spotlight_params|light_position|light_direction|light_position_object_space|light_direction_object_space|light_distance_object_space|light_position_view_space|light_direction_view_space|light_power|light_diffuse_colour_power_scaled|light_specular_colour_power_scaled|light_number|light_diffuse_colour_array|light_specular_colour_array|light_diffuse_colour_power_scaled_array|light_specular_colour_power_scaled_array|light_attenuation_array|spotlight_params_array|light_position_array|light_direction_array|light_position_object_space_array|light_direction_object_space_array|light_distance_object_space_array|light_position_view_space_array|light_direction_view_space_array|light_power_array|light_count|light_casts_shadows|ambient_light_colour|surface_ambient_colour|surface_diffuse_colour|surface_specular_colour|surface_emissive_colour|surface_shininess|derived_ambient_light_colour|derived_scene_colour|derived_light_diffuse_colour|derived_light_specular_colour|derived_light_diffuse_colour_array|derived_light_specular_colour_array|fog_colour|fog_params|camera_position|camera_position_object_space|lod_camera_position|lod_camera_position_object_space|time_0_x|costime_0_x|sintime_0_x|tantime_0_x|time_0_x_packed|time_0_1|costime_0_1|sintime_0_1|tantime_0_1|time_0_1_packed|time_0_2pi|costime_0_2pi|sintime_0_2pi|tantime_0_2pi|time_0_2pi_packed|frame_time|fps|viewport_width|viewport_height|inverse_viewport_width|inverse_viewport_height|viewport_size|texel_offsets|view_direction|view_side_vector|view_up_vector|fov|near_clip_distance|far_clip_distance|texture_viewproj_matrix|texture_viewproj_matrix_array|texture_worldviewproj_matrix|texture_worldviewproj_matrix_array|spotlight_viewproj_matrix|spotlight_worldviewproj_matrix|scene_depth_range|shadow_scene_depth_range|shadow_colour|shadow_extrusion_distance|texture_size|inverse_texture_size|packed_texture_size|pass_number|pass_iteration_number|animation_parametric|custom)\\b";
    applyHighlight(format,pattern, text);

    format.setFontWeight(QFont::Normal);
    format.setForeground(dark?QColor("lightskyblue"):Qt::blue);
    pattern = "\\b\\d+|(\\d+\\.)|(\\d+\\.\\d+)\\b";
    applyHighlight(format,pattern, text);

    if(mParent) mParent->blockSignals(false);
}

void MaterialHighlighter::applyHighlight(const QTextCharFormat &format, const QString &pattern, const QString &text)
{
    QRegularExpression expression(pattern);
    QRegularExpressionMatchIterator i = expression.globalMatch(text);
    while(i.hasNext()){
        QRegularExpressionMatch match = i.next();
        setFormat(match.capturedStart(),match.capturedLength(),format);
    }
}
