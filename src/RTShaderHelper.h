#pragma once

#include <Ogre.h>

namespace RTShaderHelper {
    void initialize(Ogre::SceneManager* sceneMgr);
    void shutdown(Ogre::SceneManager* sceneMgr);
    void applyNormalMap(Ogre::MaterialPtr& mat, const std::string& normalMapTexName);

    /// If the material's first pass is tagged with the slice E
    /// `pbr_workflow` user-binding (set by MaterialPresetLibrary's PBR
    /// templates), wire Ogre's stock SRS_COOK_TORRANCE_LIGHTING SRS
    /// for the metal-roughness workflow. Replaces slice E's FFP
    /// approximations (LBX_ADD_SIGNED metallic, LBX_MODULATE_X2
    /// roughness, etc.) with a real Cook-Torrance BRDF.
    ///
    /// The SRS expects a single packed RGBA texture: .r = AO,
    /// .g = roughness, .b = metallic (the glTF MR-texture
    /// convention). If the material has a `metallic` TUS with a
    /// texture set, that texture is used as the packed map. If only
    /// separate `roughness` / `ao` TUSs have textures, the SRS uses
    /// the default ACT_SURFACE_SPECULAR_COLOUR.xy fallback for the
    /// metal-roughness params.
    ///
    /// Returns true if the SRS was applied (workflow=metallic_roughness),
    /// false otherwise (other workflows, no tag, or PBR runtime support
    /// missing — caller falls back to slice E's FFP wiring).
    ///
    /// Specular-Glossiness and Unlit workflows are not yet supported by
    /// this helper — those keep their slice E FFP approximations.
    bool applyPbrIfTagged(Ogre::MaterialPtr& mat);

    /// Wire FFP-friendly colour operations on the slice E canonical PBR
    /// slots (albedo, ao, emissive, metallic, roughness) and mark the
    /// non-albedo slots non-FFP. This is what happens implicitly when
    /// the user clicks "Apply" in the Material Editor; calling it at
    /// import time keeps the rendered surface consistent with what the
    /// editor produces, so a freshly-imported PBR FBX doesn't render
    /// darker than the same material after a no-op Apply.
    void wirePbrSlotsForFFP(Ogre::Material* mat);

    /// FBX imports may leave a normal-map texture in the FFP multitexture chain
    /// (e.g. duplicate NormalMap units from a .material script) while RTSS also
    /// samples it via SRS_NORMALMAP — the "second layer" look in turntable/CLI.
    /// Marks every normal-related TUS non-FFP, removes duplicate units that
    /// reuse the same texture, and refreshes SRS_NORMALMAP texture_index.
    void excludeNormalMapFromFfpChain(Ogre::MaterialPtr& mat);

    /// Call after all texture units are in place (end of import / turntable).
    /// Strips normal/bump from the FFP chain, dedupes diffuse+albedo, removes
    /// stale RTSS programs, and rebuilds ShaderGenerator shading once.
    void finalizeShaderGenMaterial(Ogre::MaterialPtr& mat,
                                   const Ogre::String& normalMapTexName = {});

    /// Full viewport refresh after import (matches Material Editor Apply).
    void refreshMaterialForViewport(Ogre::MaterialPtr& mat);

    /// Resolve every TUS texture by TexturePtr (embedded FBX textures often
    /// fail RTSS name lookup across resource groups).
    void bindTextureUnitsByPointer(Ogre::MaterialPtr& mat);

    /// Full viewport sync matching Material Editor script Apply + updateMaterialText:
    /// round-trip material script, hydrate embedded textures, rebuild RTSS, rebind TUS.
    void syncMaterialForViewport(Ogre::MaterialPtr& mat);
}
