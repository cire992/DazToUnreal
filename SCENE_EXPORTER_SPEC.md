# 1-Click Scene Exporter — Design Spec

## Overview

One toolbar button in Daz Studio exports the whole scene to Unreal Engine as static meshes, with materials built and assigned automatically and no redundant assets created.

**In scope**

- Posed figures, wardrobe, hair and props, all exported as static meshes. Posed characters are baked in their current pose and position and become props.
- A single shared texture library in Unreal that mirrors the Daz runtime folder.
- Material instances organized by product and node label, created from user-assigned master materials.
- Shot folders and a level instance per export, named SceneName\_ShotName.

**Out of scope**

- Lights and cameras.
- Skeletal meshes, morph targets and animation.
- Combining the scene into a single mesh. Every figure and prop stays a separate mesh.

## Approach: fork Daz to Unreal

The exporter is a fork of the open-source Daz to Unreal bridge. It keeps the bridge's material pipeline and adds a whole-scene, static-mesh export alongside the bridge's existing per-asset workflow, which keeps working unchanged. Targets: Daz Studio 4.24 and Unreal Engine 5.8.1. The fork is based on Daz to Unreal 5.8.0.731, which builds cleanly against both; newer bridge builds have had issues with Unreal 5.8.1.

| Area | Keep from Daz to Unreal | Change or add |
| --- | --- | --- |
| Daz plugin | Toolbar integration, FBX export, material property capture | Add a separate scene export command that walks the whole scene; bake pose, morphs and subdivision into static meshes; skip lights and cameras |
| Data handoff | Material properties recorded per surface | One manifest per shot for scene exports; .dtu files unchanged for asset exports; include transforms, product names and node labels |
| Unreal materials | Master material assignment by shader type, surface name and content type; property-to-parameter matching | Material instances keyed by product and node label |
| Unreal textures | Texture import | Shared library mirroring runtime paths; overwrite on conflict; skip unchanged files |
| Unreal scene | — | Shot folders, prop reuse across shots, level instance creation |
| Settings UI | Master material settings on the Unreal side | Unchanged |

## Design decisions

**1. Material set identity: product + node label.** A material set is identified by the figure or prop's product and its node label. Michael8, Michael8\_Sunburned and Dain8 all land in the Genesis 8 Male folder, each with its own set. Re-exporting an existing label updates that set, which also updates every earlier shot that uses it. Updated sets are reported in the Unreal output log and as editor notifications.

**2. Two export paths: reusable props and baked figures.**

- **Props** are exported in local space and placed in the level instance with their Daz transforms. The same prop with the same label exports once and is placed as many times as needed, across shots.
- **Posed figures** are baked in world space at the viewport subdivision level and belong to one shot.
- A prop that is morphed or posed (a couch with a cushion morph dialed in) takes the figure path.

**3. Figures export as separate meshes per item.** Body, wardrobe, hair and geografts each export as their own mesh with a shared name prefix. This keeps the per-content-type master materials (Actor/Character, Follower/Hair, Follower/Wardrobe) working.

**4. Product lookup with a fallback.** The product comes from the Daz content database. Assets without one (custom or merged-in content) fall back to a Custom folder, subdivided by their runtime path.

**5. Deterministic path sanitizing.** Runtime paths are converted to valid Unreal paths by one fixed rule: spaces and special characters replaced, length kept under Windows limits. The same texture always maps to the same Unreal path, so the library never fills with duplicates.

**6. Overwrite, but skip unchanged files.** On conflict the runtime folder always wins. Before re-importing, the exporter compares file size and modified time against the last export and skips textures that haven't changed.

**7. Re-exporting a shot replaces it.** Exporting SceneName\_ShotName again clears and repopulates that shot's folder and level instance. Meshes deleted in Daz don't linger in Unreal. Shared textures, materials and reusable props are never cleared by a shot re-export.

**8. Prop labels carry the product name.** Props are labeled with their product name as a prefix, which keeps labels unique across products. Daz 4.24 marks duplicate labels with a space and a number in parentheses: couch02, couch02 (2), couch02 (3). The exporter strips that trailing suffix so duplicates resolve to the same prop and material set. Numbers that are part of the label itself (couch02) are kept.

**9. Shot names increment.** The shot name prompt suggests the next name in sequence (Shot01, Shot02) rather than repeating the last one, so a quick confirm never overwrites the previous shot.

**10. Scene export is optional.** The fork keeps the standard Daz to Unreal asset export intact. Scene export is its own toolbar button and its own Unreal import path, triggered only by a shot manifest, and can be turned off in settings. Keeping the scene code separate from the bridge's code also makes upstream updates easier to merge.

**11. Node instances reuse their source mesh.** A Daz node instance exports nothing of its own. Its source node's mesh is exported once and each instance is placed as another copy of it, with the instance's own transform. Instances share their source's materials, so they use the source's material set.

- An instanced prop follows the reusable prop path as usual.
- An instanced figure is baked once in its own local space instead of world space, so the same baked mesh can be placed at each instance.

## Export and import flow

1. Daz: user clicks the scene export toolbar button and confirms the suggested shot name.
2. Daz: walk the scene, skipping lights and cameras.
3. Daz: export reusable props in local space, once per label.
4. Daz: bake posed figures (pose, morphs, viewport subdivision).
5. Daz: write the shot manifest (meshes and transforms, products and labels, material properties, texture paths).
6. Unreal: sync textures into the shared library, skipping unchanged files.
7. Unreal: create or update material instances, one set per product + label.
8. Unreal: import meshes (shared props to the prop library, figures to the shot folder).
9. Unreal: build the level instance SceneName_ShotName and place every mesh.

The manifest is the only handoff: the Daz side never touches Unreal, and the Unreal side rebuilds the shot entirely from the manifest and the exported files.

## Unreal organization

Shared assets (textures, materials, reusable props) live in libraries that every export draws from; shot-specific meshes and the level instance live under the scene and shot.

```
Content/
  DazTextures/              mirrors the Daz runtime folder
    Runtime/Textures/<vendor>/<product>/...
  DazMaterials/
    Genesis 8 Male/
      Michael8/             one material set per node label
      Michael8_Sunburned/
      Dain8/
    <Prop product>/
      couch02/
      couch02_red/
    Custom/<runtime path>/  assets with no product
  DazProps/
    <Prop product>/couch02  reusable static meshes, local space
  DazScenes/
    <SceneName>/
      <ShotName>/           baked figures for this shot
      SceneName_ShotName    level instance
```

| Asset | Named by | Cleared on shot re-export |
| --- | --- | --- |
| Texture | Sanitized runtime path | No |
| Material instance | Product / node label / surface | No |
| Reusable prop mesh | Product / node label | No |
| Baked figure mesh | Scene / shot / node label / item | Yes |
| Level instance | SceneName\_ShotName | Yes, repopulated |

