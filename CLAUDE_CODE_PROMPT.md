I'm building a 1-Click Scene Exporter: a fork of the Daz to Unreal bridge that adds whole-scene export from Daz Studio into Unreal Engine as static meshes. The full design spec is in `SCENE_EXPORTER_SPEC.md` at the repo root. Read it completely before doing anything else. It is the source of truth for behavior; if anything in this prompt seems to conflict with it, ask me.

## Context

- Base: Daz to Unreal **5.8.0.731**. This version builds cleanly against my targets. Newer bridge builds have had problems with Unreal 5.8.1, so do not update or merge from upstream.
- Targets: **Daz Studio 4.24** and **Unreal Engine 5.8.1**, on Windows.
- I work mostly in Blueprints, so explain C++ changes in plain terms when you summarize them.
- You can't run Daz Studio or the Unreal Editor. I'll do the in-app testing; you build, and give me a short test checklist at each milestone.

## Hard constraints

1. **The existing Daz to Unreal asset export must keep working exactly as it does now.** I still use it constantly. Don't change its behavior, settings, file formats (.dtu) or import path.
2. Scene export is a separate feature: its own toolbar button in Daz, its own import path in Unreal triggered only by a shot manifest, and a settings toggle to turn it off.
3. Put scene-export code in new files and classes wherever possible. Where you must touch existing bridge code, keep the change small and mark it with a comment containing `SCENE_EXPORTER`, so the diff against 5.8.0.731 stays easy to review.
4. Follow the spec's decisions exactly, in particular:
   - material sets keyed by product + node label;
   - Daz's duplicate-label suffix ` (#)` (e.g. `couch02 (2)`) stripped, numbers that are part of the label kept;
   - props exported once in local space and placed by transform;
   - posed figures baked in world space at the **viewport** subdivision level;
   - node instances reusing their source mesh (an instanced figure is baked in local space);
   - re-exporting a shot clears and repopulates only that shot's folder and level instance, never shared textures, materials or props;
   - updated material sets reported in both the output log and editor notifications.

## How to work

**Step 1 — Explore and plan; write no code yet.** Map the codebase and write `IMPLEMENTATION_PLAN.md` covering:
- where the existing pieces live: toolbar action, scene/selection gathering, FBX export, material property capture, .dtu writing, and on the Unreal side the import entry point, texture import, and master material assignment;
- which of those the scene exporter will reuse as-is, call into, or need a small `SCENE_EXPORTER` hook in;
- a proposed shot manifest format (JSON), with a worked example covering a posed figure with wardrobe and hair, a reused prop placed twice, a node instance, and textures;
- the milestones below, adjusted if the code suggests a better order;
- risks and anything in the spec that the code makes hard or ambiguous.

Then stop and wait for me to review the plan.

**Step 2 — Build milestone by milestone.** After each one: make sure both the Daz plugin and the Unreal plugin compile, summarize what changed, give me the in-app test checklist, and wait for my results before starting the next.

## Milestones

1. **Scaffolding:** the scene export toolbar button in Daz, the settings toggle, the shot name prompt suggesting the next name in sequence (Shot01, Shot02…), and a scene walker that skips lights and cameras and writes a manifest with no meshes yet.
2. **Daz mesh export:** props in local space (deduplicated by product + cleaned label), posed figures baked to static meshes per item (body, wardrobe, hair, geografts), node instances resolved to their source, and material properties plus texture paths in the manifest.
3. **Unreal texture sync:** the shared texture library mirroring runtime paths, with deterministic path sanitizing, overwrite on conflict, and skipping unchanged files by size and modified time.
4. **Unreal materials:** material instances under product / label from the existing master material settings, property-to-parameter matching, and update reporting.
5. **Unreal mesh import:** reusable props to the prop library, baked figures to the shot folder, each a separate mesh.
6. **Level instance:** create or repopulate `SceneName_ShotName` and place every mesh with its transform, including multiple placements of reused props and instances.
7. **Hardening:** a re-export test, products missing from the content database (Custom fallback), long or odd paths, and a final regression pass confirming the standard asset export is unchanged.

Start with Step 1.
