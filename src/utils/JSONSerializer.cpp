// license:GPLv3+

#include "core/stdafx.h"

#include "JSONSerializer.h"
#include "miniz/miniz.h"


static const ankerl::unordered_dense::map<int, string> jsonTagToName {
   // Shared fields
   { FID(PIID), "id" },
   { FID(NAME), "name" },
   { FID(VCEN), "center" },
   { FID(VPOS), "position" },
   { FID(VSIZ), "size" },
   { FID(WDTH), "width" },
   { FID(WDTP), "width_top" }, // (thickness for rubber)
   { FID(HIGH), "height" },
   { FID(HTBT), "height_bottom" },
   { FID(HTTP), "height_top" },
   { FID(LGTH), "length" }, // Gate & Spinner
   { FID(ROTA), "rotation" },
   { FID(ROTZ), "rot_z" },
   { FID(TYPE), "type" },
   { FID(TMON), "timer_enabled" },
   { FID(TMIN), "timer_interval" },
   { FID(IMAG), "image" },
   { FID(MATR), "material" },
   { FID(SURF), "surface" },
   { FID(DSPT), "ui_show_texture" },
   { FID(DILT), "disable_lighting_top" },
   { FID(DILB), "disable_lighting_below" },
   { FID(REEN), "reflection_enabled" },
   { FID(MAPH), "physics_material" },
   { FID(OVPH), "overwrite_physics" },
   { FID(CLDR), "collidable" },
   { FID(THRS), "threshold" },
   { FID(HTEV), "hitEvent" },
   { FID(ELAS), "elasticity" },
   { FID(ELFO), "elasticity_falloff" },
   { FID(AFRC), "damping" }, // Gate & Spinner
   { FID(RFCT), "friction" },
   { FID(RSCT), "scatter" },
   { FID(COLR), "color" },
   { FID(FALP), "alpha" },
   { FID(TRNS), "transparent" },
   { FID(TEXT), "text" },
   { FID(ADDB), "add_blend" },
   { FID(LMAP), "lightmap" },
   { FID(ALGN), "alignment" },
   { FID(PIDB), "depth_bias" }, // Depth Bias for HitTarget and Primitive (others have a dedicated id)
   { FID(RVIS), "visible" }, // Render visibility for Ramp and Rubbers
   { FID(TVIS), "visible" }, // Render visibility for HitTarget and Primitive
   { FID(VSBL), "visible" }, // Render visibility for others
   { FID(UPTM), "update_interval" }, // light Sequencer & Disp Reel
   { FID(SCOL), "side_color" }, // side color of Primitive but collection count of PinTable
   { FID(SIMG), "side_image" }, // side image of Primitive but image count of PinTable
   { FID(PNTS), "dragpoints" },
   { FID(RADI), "radius" },
   { FID(VER1), "v1" },
   { FID(VER2), "v2" },
   { FID(CLRB), "back_color" },
   { FID(EBLD), "enabled" }, // for Kicker and Trigger
   { FID(LEMO), "legacy_mode" }, // for Kicker and HitTarget
   { FID(FORC), "force" }, // actual force for Bumper but mass for Flipper...
   { FID(BGLS), "desktop_backdrop" },
   // ISelect
   { FID(LOCK), "ui_locked" },
   { FID(LVIS), "ui_visible" },
   { FID(LAYR), "legacy_layer" },
   { FID(LANR), "named_layer" },
   { FID(GRUP), "part_group" },
   // Ball
   { FID(MASS), "mass" },
   { FID(FREF), "forceReflection" },
   { FID(DCMD), "decalMode" },
   { FID(DIMG), "imageDecal" },
   { FID(BISC), "bulb_intensity_scale" },
   { FID(PFRF), "playfieldReflectionStrength" },
   { FID(SPHR), "pinballEnvSphericalMapping" },
   // Bumper
   { FID(BSCT), "scatter" },
   { FID(HISC), "height_scale" },
   { FID(RISP), "ring_speed" },
   { FID(ORIN), "orientation" },
   { FID(RDLI), "ring_drop_offset" },
   { FID(BAMA), "base_material" },
   { FID(SKMA), "skirt_material" },
   { FID(RIMA), "ring_material" },
   { FID(CAVI), "cap_visible" },
   { FID(BSVS), "base_visible" },
   { FID(RIVS), "ring_visible" },
   { FID(SKVS), "skirt_visible" },
   { FID(HAHE), "hit_event" },
   { FID(COLI), "collidable" },
   // Decal
   { FID(SIZE), "sizingtype" },
   { FID(VERT), "vertical_text" },
   // DispReel
   { FID(SOUN), "sound" },
   { FID(RCNT), "reel_count" },
   { FID(RSPC), "reel_spacing" },
   { FID(MSTP), "motor_steps" },
   { FID(RANG), "digit_range" },
   { FID(UGRD), "use_image_grid" },
   { FID(VISI), "visible" },
   { FID(GIPR), "images_per_grid_row" },
   // Flipper
   { FID(BASR), "base_radius" },
   { FID(ENDR), "end_radius" },
   { FID(FLPR), "flipper_radius_max" },
   { FID(FRTN), "return" },
   { FID(ANGS), "start_angle" },
   { FID(ANGE), "end_angle" },
   { FID(OVRP), "override_physics" },
   { FID(RUMA), "rubber_material" },
   { FID(RTHK), "rubber_thickness_legacy" }, //!! deprecated, remove
   { FID(RTHF), "rubber_thickness" },
   { FID(RHGT), "rubber_height_legacy" }, //!! deprecated, remove
   { FID(RHGF), "rubber_height" },
   { FID(RWDT), "rubber_width_legacy" }, //!! deprecated, remove
   { FID(RWDF), "rubber_width" },
   { FID(STRG), "strength" },
   { FID(FRIC), "friction" },
   { FID(RPUP), "ramp_up" },
   { FID(SCTR), "scatter" },
   { FID(TODA), "torque_damping" },
   { FID(TDAA), "torque_damping_angle" },
   { FID(ENBL), "enabled" },
   { FID(FRMN), "flipper_radius_min" },
   { FID(FHGT), "height" },
   // Gate
   { FID(HGTH), "height" },
   { FID(GSUP), "show_bracket" },
   { FID(GCOL), "collidable" },
   { FID(GAMA), "angle_max" },
   { FID(GAMI), "angle_min" },
   { FID(GFRC), "friction" },
   { FID(GGFC), "gravity_factor" },
   { FID(GVSB), "visible" },
   { FID(TWWA), "two_way" },
   { FID(GATY), "type" },
   // HitTarget
   { FID(TRTY), "target_type" },
   { FID(ISDR), "is_dropped" },
   { FID(DRSP), "drop_speed" },
   { FID(RADE), "raise_delay" },
   // Kicker
   { FID(KSCT), "scatter" },
   { FID(KHAC), "hit_accuracy" },
   { FID(KHHI), "hit_height" },
   { FID(KORI), "orientation" },
   { FID(FATH), "fall_through" },
   // Light
   { FID(HGHT), "height" },
   { FID(FAPO), "falloff_power" },
   { FID(STAT), "state_legacy" }, //!! deprecated, remove as soon as increasing file version to 10.9+
   { FID(STTF), "state" },
   { FID(COL2), "color2" },
   { FID(BPAT), "blink_pattern" },
   { FID(IMG1), "image" },
   { FID(BINT), "blink_interval" },
   { FID(BWTH), "intensity" },
   { FID(TRMS), "transmission_scale" },
   { FID(LIDB), "depth_bias" },
   { FID(FASP), "fade_speed_up" },
   { FID(FASD), "fade_speed_down" },
   { FID(BULT), "bulb_light" },
   { FID(IMMO), "image_mode" },
   { FID(SHBM), "show_bulb_mesh" },
   { FID(STBM), "static_bulb_mesh" },
   { FID(SHRB), "show_reflection_on_ball" },
   { FID(BMSC), "mesh_radius" },
   { FID(BMVA), "modulate_vs_add" },
   { FID(BHHI), "bulb_halo_height" },
   { FID(SHDW), "shadows" },
   { FID(FADE), "fader" },
   // LightSeq
   { FID(COLC), "collection" },
   { FID(CTRX), "center_x" },
   { FID(CTRY), "center_y" },
   // PartGroup
   { FID(PMSK), "player_mode_visibility_mask" },
   { FID(SPRF), "space_reference" },
   // PinTable
   { FID(LEFT), "left" },
   { FID(TOPX), "top" },
   { FID(RGHT), "right" },
   { FID(BOTM), "bottom" },
   { FID(EFSS), "is_FSS_view_mode_enabled" },
   { FID(VSM0), "desktop_mode" },
   // { FID(ROTA), "desktop_rotation" }, // shared with others as 'rotation'
   { FID(INCL), "desktop_inclination" },
   { FID(LAYB), "desktop_layback" },
   { FID(FOVX), "desktop_fov" },
   { FID(XLTX), "desktop_view_x" },
   { FID(XLTY), "desktop_view_y" },
   { FID(XLTZ), "desktop_view_z" },
   { FID(SCLX), "desktop_scale_x" },
   { FID(SCLY), "desktop_scale_y" },
   { FID(SCLZ), "desktop_scale_z" },
   { FID(HOF0), "desktop_horizontal_ofs" },
   { FID(VOF0), "desktop_vertical_ofs" },
   { FID(WTZ0), "desktop_window_top_z_ofs" },
   { FID(WBZ0), "desktop_window_bot_z_ofs" },
   { FID(VSM1), "fullscreen_mode" },
   { FID(ROTF), "fullscreen_rotation" },
   { FID(INCF), "fullscreen_inclination" },
   { FID(LAYF), "fullscreen_layback" },
   { FID(FOVF), "fullscreen_fov" },
   { FID(XLFX), "fullscreen_view_x" },
   { FID(XLFY), "fullscreen_view_y" },
   { FID(XLFZ), "fullscreen_view_z" },
   { FID(SCFX), "fullscreen_scale_x" },
   { FID(SCFY), "fullscreen_scale_y" },
   { FID(SCFZ), "fullscreen_scale_z" },
   { FID(HOF1), "fullscreen_horizontal_ofs" },
   { FID(VOF1), "fullscreen_vertical_ofs" },
   { FID(WTZ1), "fullscreen_window_top_z_ofs" },
   { FID(WBZ1), "fullscreen_window_bot_z_ofs" },
   { FID(VSM2), "fss_mode" },
   { FID(ROFS), "fss_rotation" },
   { FID(INFS), "fss_inclination" },
   { FID(LAFS), "fss_layback" },
   { FID(FOFS), "fss_fov" },
   { FID(XLXS), "fss_view_x" },
   { FID(XLYS), "fss_view_y" },
   { FID(XLZS), "fss_view_z" },
   { FID(SCXS), "fss_scale_x" },
   { FID(SCYS), "fss_scale_y" },
   { FID(SCZS), "fss_scale_z" },
   { FID(HOF2), "fss_horizontal_ofs" },
   { FID(VOF2), "fss_vertical_ofs" },
   { FID(WTZ2), "fss_window_top_z_ofs" },
   { FID(WBZ2), "fss_window_bot_z_ofs" }, //
   { FID(ORRP), "override_physics" },
   { FID(ORPF), "override_physics_flipper" },
   { FID(GAVT), "gravity" },
   { FID(FRCT), "friction" },
   { FID(ELFA), "elasticity_falloff" },
   { FID(PFSC), "scatter" },
   { FID(SCAT), "default_scatter" },
   { FID(NDGT), "nudge_time" },
   { FID(PHML), "physics_max_loops" },
   { FID(REEL), "render_EM_reels" },
   { FID(DECL), "render_decals" },
   { FID(OFFX), "win_editor_view_offset_x" },
   { FID(OFFY), "win_editor_view_offset_y" },
   { FID(ZOOM), "win_editor_zoom" },
   { FID(SLPX), "angle_tilt_max" },
   { FID(SLOP), "angle_tilt_min" },
   { FID(BIMG), "backdrop_image_0" },
   { FID(BIMF), "backdrop_image_1" },
   { FID(BIMS), "backdrop_image_2" },
   { FID(BIMN), "image_backdrop_night_day" },
   { FID(IMCG), "image_color_grade" },
   { FID(BLIM), "ball_image" },
   { FID(BLSM), "ball_spherical_mapping" },
   { FID(BLIF), "ball_image_decal" },
   { FID(EIMG), "env_image" },
   { FID(NOTX), "notes_text" },
   { FID(SSHT), "screenshot" },
   { FID(FBCK), "win_editor_backdrop" },
   { FID(GLAS), "glass_top_height" },
   { FID(GLAB), "glass_bottom_height" },
   { FID(PLMA), "playfield_material" },
   { FID(BCLR), "color_backdrop" },
   { FID(TDFT), "difficulty" },
   { FID(LZAM), "light_ambient" },
   { FID(LZDI), "light_emission" },
   { FID(LZHI), "light_height" },
   { FID(LZRA), "light_range" },
   { FID(LIES), "light_emission_scale" },
   { FID(ENES), "env_emission_scale" },
   { FID(GLES), "global_emission_scale" },
   { FID(AOSC), "AO_scale" },
   { FID(SSSC), "SSR_scale" },
   { FID(CLBH), "ground_to_lockbar_height" },
   { FID(SVOL), "table_sound_volume" },
   { FID(MVOL), "table_music_volume" },
   { FID(PLST), "playfield_reflection_strength" },
   { FID(BDMO), "ball_decal_mode" },
   { FID(BPRS), "ball_playfield_reflection_strength" },
   { FID(DBIS), "default_bulb_intensity_scale_on_ball" },
   { FID(GDAC), "ui_editor_grid" },
   { FID(UAOC), "enable_AO" },
   { FID(USSR), "enable_SSR" },
   { FID(TMAP), "tonemapper" },
   { FID(EXPO), "exposure" },
   { FID(BLST), "bloom_strength" },
   { FID(MASI), "materials_legacy_count" },
   { FID(PHMA), "plysic_materials" },
   { FID(RPRB), "renderprobes" },
   { FID(SEDT), "part_count" },
   { FID(SSND), "sound_count" },
   { FID(SIMG), "image_count" },
   { FID(SFNT), "font_count" },
   { FID(CODE), "vbs_script" },
   { FID(CCUS), "ui_custom_colors" },
   { FID(TLCK), "tablelocked" },
   // Plunger
   { FID(ZADJ), "z_adjust" },
   { FID(HPSL), "stroke" },
   { FID(SPDP), "speed_pull" },
   { FID(SPDF), "speed_fire" },
   { FID(ANFR), "anim_frames" },
   { FID(MEST), "mech_strength" },
   { FID(MECH), "mech_plunger" },
   { FID(APLG), "auto_plunger" },
   { FID(MPRK), "park_position" },
   { FID(PSCV), "scatter_velocity" },
   { FID(MOMX), "momentum_transfer" },
   { FID(TIPS), "tip_shape" },
   { FID(RODD), "rod_diam" },
   { FID(RNGG), "ring_gap" },
   { FID(RNGD), "ring_diam" },
   { FID(RNGW), "ring_width" },
   { FID(SPRD), "spring_diam" },
   { FID(SPRG), "spring_gauge" },
   { FID(SPRL), "spring_loops" },
   { FID(SPRE), "spring_end_loops" },
   // Ramp
   { FID(WDBT), "width_bottom" },
   { FID(IMGW), "image_walls" },
   { FID(WLHL), "left_wall_height" },
   { FID(WLHR), "right_wall_height" },
   { FID(WVHL), "left_wall_height_visible" },
   { FID(WVHR), "right_wall_height_visible" },
   { FID(RADB), "depth_bias" },
   { FID(RADX), "wire_distance_x" },
   { FID(RADY), "wire_distance_y" },
   // Rubber
   { FID(HTHI), "hit_height" },
   { FID(ESTR), "static_rendering" },
   { FID(ESIE), "show_in_editor" },
   { FID(ROTX), "rot_x" },
   { FID(ROTY), "rot_y" },
   // Spinner
   { FID(SMAX), "angle_max" },
   { FID(SMIN), "angle_min" },
   { FID(SELA), "elasticity" },
   { FID(SVIS), "visible" },
   { FID(SSUP), "show_bracket" },
   { FID(IMGF), "image" },
   // TextBox
   { FID(CLRF), "fontcolor" },
   { FID(INSC), "intensity_scale" },
   { FID(IDMD), "dmd" },
   { FID(FONT), "font" },
   // Timer
   // Trigger
   { FID(WITI), "wire_thickness" },
   { FID(SCAX), "scale_x" },
   { FID(SCAY), "scale_y" },
   { FID(THOT), "hit_height" },
   { FID(SHAP), "shape" },
   { FID(ANSP), "anim_speed" },
   // Dragpoint
   { FID(POSZ), "position_z" },
   { FID(SMTH), "smooth" },
   { FID(SLNG), "slingshot" },
   { FID(ATEX), "auto_texture" },
   { FID(TEXC), "texture_coordinate" },
   // Flasher
   { FID(FLAX), "flasher_x" }, // FIXME remove
   { FID(FLAY), "flasher_y" }, // FIXME remove
   { FID(FHEI), "height" },
   { FID(FROX), "rotation_x" },
   { FID(FROY), "rotation_y" },
   { FID(FROZ), "rotation_z" },
   { FID(IMAB), "image_b" },
   { FID(MOVA), "modulate_vs_add" },
   { FID(FVIS), "visible" },
   { FID(RDMD), "render_mode" },
   { FID(RSTL), "render_style" },
   { FID(GRGH), "glass_roughness" },
   { FID(GAMB), "glass_ambient" },
   { FID(GTOP), "glass_pad_top" },
   { FID(GBOT), "glass_pad_bottom" },
   { FID(GLFT), "glass_pad_left" },
   { FID(GRHT), "glass_pad_right" },
   { FID(LINK), "image_src_link" },
   { FID(FLDB), "depth_bias" },
   { FID(FILT), "filter_type" },
   { FID(FIAM), "filter_amount" },
   { FID(DPNT), "dragpoints" },
   // Primitive
   { FID(RTV0), "prim_rot1_x" },
   { FID(RTV1), "prim_rot1_y" },
   { FID(RTV2), "prim_rot1_z" },
   { FID(RTV3), "prim_trans_x" },
   { FID(RTV4), "prim_trans_y" },
   { FID(RTV5), "prim_trans_z" },
   { FID(RTV6), "prim_rot2_x" },
   { FID(RTV7), "prim_rot2_y" },
   { FID(RTV8), "prim_rot2_z" },
   { FID(NRMA), "normal_map" },
   { FID(SIDS), "sides" },
   { FID(DTXI), "draw_textures_inside" },
   { FID(EFUI), "ui_edge_fFactor" },
   { FID(CORF), "collision_reduction_factor" },
   { FID(ISTO), "toy" },
   { FID(U3DM), "use_mesh" },
   { FID(STRE), "static_rendering" },
   { FID(EBFC), "backfaces_enabled" },
   { FID(DIPT), "display_texture" },
   { FID(OSNM), "object_space_normal_map" },
   { FID(ZMSK), "use_depth_mask" },
   { FID(REFL), "reflection_probe" },
   { FID(RSTR), "reflection_strength" },
   { FID(REFR), "refraction_probe" },
   { FID(RTHI), "refraction_thickness" },
   // Surface
   { FID(DROP), "droppable" },
   { FID(FLIP), "flipbook" },
   { FID(ISBS), "is_bottom_solid" },
   { FID(CLDW), "collidable" },
   { FID(SIMG), "side_image" },
   { FID(SIMA), "side_material" },
   { FID(TOMA), "top_material" },
   { FID(SLMA), "sling_shot_material" },
   { FID(SLGF), "slingshot_force" },
   { FID(SLTH), "slingshot_threshold" },
   { FID(WFCT), "friction" },
   { FID(WSCT), "scatter" },
   { FID(SLGA), "slingshot_animation" },
   { FID(SVBL), "side_visible" },
};
static ankerl::unordered_dense::map<string, int> ReverseMap(const ankerl::unordered_dense::map<int, string>& map)
{
   ankerl::unordered_dense::map<string, int> result;
   for (const auto& [tag, name] : map)
      result[name] = tag;
   return result;
}
static const ankerl::unordered_dense::map<string, int> jsonNameToTag = ReverseMap(jsonTagToName);

const string& JSONSerializer::GetFieldName(int fieldId)
{
   const auto it = jsonTagToName.find(fieldId);
   assert(it != jsonTagToName.end());
   return it->second;
}

int JSONSerializer::GetFieldId(const string& filedName)
{
   const auto it = jsonNameToTag.find(filedName);
   assert(it != jsonNameToTag.end());
   return it->second;
}


class FilesystemSerializer final : public JSONSerializer::Serializer
{
public:
   explicit FilesystemSerializer(const std::filesystem::path& basePath)
      : m_basePath(basePath)
   {
   }

   void AddTextFile(const std::filesystem::path& path, const std::string& content) override
   {
      std::filesystem::path fullPath = m_basePath / path;
      std::filesystem::create_directories(fullPath.parent_path());
      std::ofstream out(fullPath, std::ios::binary);
      out << content;
   }

   void AddBinaryFile(const std::filesystem::path& path, const std::vector<uint8_t>& data) override
   {
      std::filesystem::path fullPath = m_basePath / path;
      std::filesystem::create_directories(fullPath.parent_path());
      std::ofstream out(fullPath, std::ios::binary);
      out.write(reinterpret_cast<const char*>(data.data()), data.size());
   }

private:
   std::filesystem::path m_basePath;
};

class ZipSerializer : public JSONSerializer::Serializer
{
public:
   explicit ZipSerializer(const std::filesystem::path& zipPath)
      : m_zipPath(zipPath)
   {
      memset(&m_zipArchive, 0, sizeof(m_zipArchive));
   }

   ~ZipSerializer()
   {
      if (m_zipArchive.m_pState)
      {
         mz_zip_writer_finalize_archive(&m_zipArchive);
         mz_zip_writer_end(&m_zipArchive);
         //mz_zip_writer_delete(&m_zipArchive);
      }
   }

   void AddTextFile(const std::filesystem::path& path, const std::string& content) override
   {
      if (!m_zipArchive.m_pState)
      {
         if (!mz_zip_writer_init_file(&m_zipArchive, m_zipPath.string().c_str(), 0))
         {
            throw std::runtime_error("Failed to initialize zip writer");
         }
      }
      mz_zip_writer_add_mem(&m_zipArchive, path.string().c_str(), content.data(), content.size(), MZ_BEST_COMPRESSION);
   }

   void AddBinaryFile(const std::filesystem::path& path, const std::vector<uint8_t>& data) override
   {
      if (!m_zipArchive.m_pState)
      {
         if (!mz_zip_writer_init_file(&m_zipArchive, m_zipPath.string().c_str(), 0))
         {
            throw std::runtime_error("Failed to initialize zip writer");
         }
      }
      mz_zip_writer_add_mem(&m_zipArchive, path.string().c_str(), data.data(), data.size(), MZ_BEST_COMPRESSION);
   }

private:
   std::filesystem::path m_zipPath;
   mz_zip_archive m_zipArchive;
};

std::unique_ptr<JSONSerializer::Serializer> JSONSerializer::Create(const std::filesystem::path& path)
{
   if (std::filesystem::exists(path) && std::filesystem::is_directory(path))
      return std::make_unique<FilesystemSerializer>(path);
   else
      return std::make_unique<ZipSerializer>(path);
   // FIXME create the root format info file (format version and any other needed information)
}


JSONObjectWriter::JSONObjectWriter(Serializer& serializer, const std::filesystem::path& path)
   : m_json()
{
}

JSONObjectWriter::JSONObjectWriter(nlohmann::ordered_json& array)
   : m_json()
   , m_array(array)
{
}

void JSONObjectWriter::BeginObject(const int objectId, bool isArray, bool isSkippable)
{
   /*std::unique_ptr<IPartWriter> JSONObjectWriter::WriteArrayElement(const int fieldId)
{
   auto &obj = m_json[GetFieldName(fieldId)];
   return std::make_unique<JSONObjectWriter>(obj);
}*/
}

void JSONObjectWriter::WriteBool(const int fieldId, const bool value) { m_json[GetFieldName(fieldId)] = value; }

void JSONObjectWriter::WriteInt(const int fieldId, const int value) { m_json[GetFieldName(fieldId)] = value; }

void JSONObjectWriter::WriteUInt(const int fieldId, const unsigned int value) { m_json[GetFieldName(fieldId)] = value; }

void JSONObjectWriter::WriteFloat(const int fieldId, const float value) { m_json[GetFieldName(fieldId)] = value; }

void JSONObjectWriter::WriteString(const int fieldId, const string& value) { m_json[GetFieldName(fieldId)] = value; }

void JSONObjectWriter::WriteWideString(const int fieldId, const wstring& value) { m_json[GetFieldName(fieldId)] = MakeString(value); }

void JSONObjectWriter::WriteVector2(const int fieldId, const Vertex2D& vec)
{
   auto& obj = m_json[GetFieldName(fieldId)];
   obj["x"] = vec.x;
   obj["y"] = vec.y;
}

void JSONObjectWriter::WriteVector3(const int fieldId, const vec3& vec)
{
   auto& obj = m_json[GetFieldName(fieldId)];
   obj["x"] = vec.x;
   obj["y"] = vec.y;
   obj["z"] = vec.z;
}

void JSONObjectWriter::WriteVector4(const int fieldId, const vec4& vec)
{
   auto& obj = m_json[GetFieldName(fieldId)];
   obj["x"] = vec.x;
   obj["y"] = vec.y;
   obj["z"] = vec.z;
   obj["w"] = vec.w;
}

void JSONObjectWriter::WriteScript(int fieldId, const string& value)
{
   // FIXME save as a separate file (name of the table.vbs)
}

void JSONObjectWriter::WriteFontDescriptor(int fieldId, const FontDesc& value)
{
   auto& obj = m_json[GetFieldName(fieldId)];
   obj["name"] = value.name;
   obj["size"] = value.size;
   obj["weight"] = value.weight;
   obj["charset"] = value.charset;
   obj["italic"] = (value.attributes & 0x02) != 0;
   obj["underline"] = (value.attributes & 0x04) != 0;
   obj["strikethrough"] = (value.attributes & 0x08) != 0;
}

void JSONObjectWriter::WriteRaw(const int fieldId, const void* pvalue, const int size)
{
   // FIXME implement, knowing that this is used in:
   // - MATE & PHMA in PinTable for pre 10.8 materials => just discard as this is pre 10.8
   // - DATA in PinBinary for sound and image => directly save image & sounds as binary file in sub folders
   // - M3.. in Primitive for mesh => save as binary GLTF in a Meshes sub folder => add a dedicated method ?
}

void JSONObjectWriter::EndObject()
{
   if (m_array)
   {
      m_array->get().push_back(m_json);
   }
   else
   {
      PLOGD << m_json.dump(2);
   }
}
