# 1-Click Scene Exporter — Implementation Plan

Status: **Step 1 (plan only, no code yet).** Waiting for review.

Source of truth: `SCENE_EXPORTER_SPEC.md`. This plan maps the spec onto the existing code, says what gets reused or hooked, defines the shot manifest, and lists risks and open questions. The questions that change what I build are in section 8.

---

## 0. Baseline and build constraints

| Item | What the repo shows |
| --- | --- |
| Unreal plugin | `UnrealPlugin/DazToUnreal/DazToUnreal.uplugin`: `VersionName` 5.8.0, `EngineVersion` 5.8.0 |
| Daz plugin | `DazStudioPlugin/version.h`: 2023.1.0.53, "Vod's Version" |
| Shared bridge library | `dzbridge-common` is a **git submodule** of `github.com/David-Vodhanel/DazBridgeUtils`, pinned at `718b8f5` |

Consequences:

- **Nothing in `dzbridge-common` will be modified.** Commits there would belong to a repo outside this one and would not travel with your fork. The scene exporter subclasses the bridge classes and calls their public and protected helpers instead.
- **I can't compile either plugin in this environment.** It has no Daz Studio SDK, Qt 4.8, FBX SDK or Unreal Engine 5.8.1. I'll write the code against the APIs already used in this repo, keep it Qt 4 compatible (for example the `DTU_REGEX` macro rather than `QRegularExpression`), and avoid new third-party dependencies. At each milestone you compile both plugins and send me any errors, and I fix them before we move on. Section 8, question 1 asks you to confirm this workflow.
- The Unreal plugin binary is also shipped inside the Daz plugin as embedded zips (`DazStudioPlugin/Resources/UE*.zip`). Those are older engine versions and stay untouched. You install the forked Unreal plugin by copying `UnrealPlugin/DazToUnreal` into the project's `Plugins` folder, as you do now.

---

## 1. Where the existing pieces live

### Daz Studio side

| Piece | Location | Notes |
| --- | --- | --- |
| Action registration | `DazStudioPlugin/pluginmain.cpp:28` (`DZ_PLUGIN_CLASS_GUID(DzUnrealAction, …)`) | Daz builds the menu item from this. |
| Menu path / action group | `dzbridge-common/include/DzBridgeAction.h:154-155` | `File/Send To`, group "Bridges". |
| Toolbar action entry | `DazStudioPlugin/DzUnrealAction.cpp:36` (ctor, icon), `:57` `executeAction()` | Shows the bridge dialog, then calls `exportHD()`. |
| Selection gathering | `DzBridgeAction.cpp:5090` `buildRootNodeList()`, `:2052` `getScenePropList()` | Selection-based. There's no whole-scene walker beyond the Environment path. |
| Existing "Environment" export | `DzBridgeAction.cpp:841-905` (`exportAsset`), `:3269` `writeEnvironment`, `:3283` `writeInstances`, `:3318` `writeInstance` | Closest relative of the scene exporter: it exports each prop type once at the origin and writes instance transforms, including `DzInstanceNode`. It keys by asset ID rather than label, uses Euler angles, and has no product lookup, figure baking or material sets. **We learn from it but don't call it.** |
| FBX export | `DzBridgeAction.cpp:1076` `exportNode()` (uses `DzFbxExporter`), `:705` `exportHD()`, `:4226` `postProcessFbx()` | Exports skinned meshes in bind pose plus skeleton. That's right for skeletal meshes, wrong for baked statics. |
| FBX SDK wrapper | `dzbridge-common/include/OpenFBXInterface.h` | `CreateScene` / `SaveScene`. **We reuse this to write our own static FBX files.** |
| Material property capture | `DzBridgeAction.cpp:2504` `writeAllMaterials`, `:2569` `startMaterialBlock`, `:2621` `writeMaterialProperty` | Writes the "Version 4" material block the Unreal side already understands, including `Asset Type` = presentation/content type. |
| Temporary textures | `DzBridgeAction.cpp:2383` `exportAssetWithDtu()` | Copies textures that live in the Daz temp folder (layered images and the like) next to the export. |
| Normal-from-height generation | `DzBridgeAction.cpp:160` `preProcessScene()` | Optional. Not used by scene export at first (see question 8.8). |
| `.dtu` writing | `DazStudioPlugin/DzUnrealAction.cpp:191` `writeConfiguration()`, `DzBridgeAction.cpp:2465` `writeDTUHeader()` | **Untouched.** |
| UDP notify to Unreal | `DzUnrealAction.cpp:174` and `:290` | Sends the file path as one datagram to `127.0.0.1:<port>`. |
| Settings storage | `DzUnrealDialog.cpp:87` `QSettings("Daz 3D","DazToUnreal")`, `:227` `loadSavedSettings()` | Holds `Port` and `IntermediatePath`, which the scene exporter reads too. |

### Unreal side

| Piece | Location | Notes |
| --- | --- | --- |
| Module start, UDP listener | `Private/DazToUnreal.cpp:155` `StartupModule`, `:350` `StartupUDPListener` | |
| Import entry point | `Private/DazToUnreal.cpp:395` `Tick()`. Receives the datagram at `:465`, parses JSON, calls `ImportFromDaz()` at `:495` | **This is the single place a manifest gets routed.** |
| Asset import | `Private/DazToUnreal.cpp:522` `ImportFromDaz()` (~1,300 lines) | Material parsing is inline at `:832-1110`. |
| Environment level build | `Private/DazToUnreal.cpp:691`, `Private/DazToUnrealEnvironment.cpp` | Spawns actors into a new level. Its axis and rotation conversion is a useful reference. |
| Texture import | `Private/DazToUnreal.cpp:1590-1615` (copies into the per-asset folder), `:1897` `ImportTextureAssets()`, sRGB fix-up in `Tick()` | Per-character texture folder, no shared library. |
| Master material choice | `Private/DazToUnrealMaterials.cpp:32` `GetBaseMaterial()` (by shader type, content type and surface-name suffix), `Public/DazToUnrealSettings.h:343` `FindMaterial()` (Material Packs, then built-in defaults) | **Reused as-is.** |
| Material instance creation | `Private/DazToUnrealMaterials.cpp:209` `CreateMaterial()`, `:492` `CorrectDazShaders()`; settings `MaterialPacks` (`DazToUnrealSettings.h:202`), `MaterialPropertyMapping` (`:206`) | **Reused as-is.** It already accepts a full texture object path as a property value (`:440-446`), which suits a shared texture library. |
| Intermediate (parent) materials | `Private/DazToUnreal.cpp:1616-1708` | Per-object "_BaseMat" grouping. Not used by scene export at first (see risk R6). |
| FBX import | `Private/DazToUnreal.cpp:1990` `ImportFBXAsset()` | Skeletal-oriented. Scene export uses its own static-mesh import task. |
| Name sanitizing | `Private/DazToUnrealUtils.cpp:30` `SanitizeName()` | Not path-safe (it leaves `,` `#` `@` and others). Scene export gets its own path rule. |

---

## 2. Reuse, call into, or hook

**Reuse as-is (no changes):** `DzBridgeAction` material capture (`startMaterialBlock`, `writeMaterialProperty`, `finishMaterialBlock`, `exportAssetWithDtu`, `isGeograft`, `getMD5`), `OpenFBXInterface`, `FDazToUnrealMaterials::CreateMaterial` / `GetBaseMaterial` / `CorrectDazShaders`, `UDazToUnrealSettings::FindMaterial` and the Material Pack settings.

**New code (new files only):**

Daz side (`DazStudioPlugin/SceneExporter/`):

- `DzUnrealSceneAction.{h,cpp}`: the toolbar action. It subclasses `DzBridgeAction` only to reach the material helpers, overrides `executeAction()`, and never calls `exportHD()`, `exportAsset()` or `writeConfiguration()`.
- `DzSceneShotDialog.{h,cpp}`: the scene name and shot name prompt.
- `DzSceneWalker.{h,cpp}`: classifies every node as figure, follower, prop, instance, group, or skip.
- `DzSceneNaming.{h,cpp}`: label cleanup, product lookup with fallback, next-shot-name logic.
- `DzSceneMeshWriter.{h,cpp}`: writes a static FBX from a node's cached (posed, morphed, subdivided) geometry through `OpenFBXInterface`.
- `DzSceneManifestWriter.{h,cpp}`: the JSON writer, using `DzJsonWriter` like the `.dtu`.

Unreal side (`Source/DazToUnreal/{Public,Private}/SceneExporter/`):

- `DazSceneExporterSettings`: a new `UDeveloperSettings` in Project Settings → Plugins → "Daz Scene Exporter". Holds the on/off toggle and the library root folders. `UDazToUnrealSettings` is not touched.
- `DazSceneManifest`: parses the manifest into structs and validates its version.
- `DazScenePaths`: the single deterministic sanitizing rule (spec decision 5).
- `DazSceneTextureSync`: the shared texture library, the skip-unchanged ledger, and sRGB fix-ups.
- `DazSceneMaterials`: material sets by product and label, in-place updates, change reporting.
- `DazSceneMeshImport`: static-mesh FBX import, slot-to-material assignment, skipping props whose FBX hasn't changed.
- `DazSceneLevelInstance`: creates or repopulates the `SceneName_ShotName` level.
- `DazSceneImporter`: runs the steps in order with a progress dialog, then logs and posts a notification.

**`SCENE_EXPORTER` hooks in existing files (the whole diff against the base):**

| File | Change | Size |
| --- | --- | --- |
| `DazStudioPlugin/pluginmain.cpp` | `#include` plus one `DZ_PLUGIN_CLASS_GUID(DzUnrealSceneAction, <new guid>)` | 2 lines |
| `DazStudioPlugin/CMakeLists.txt` | add the new source files | ~12 lines |
| `DazStudioPlugin/DzUnrealDialog.{h,cpp}` | an "Enable Scene Export" checkbox in Advanced Settings, saved as `SceneExportEnabled` in the same QSettings | ~15 lines |
| `UnrealPlugin/.../Private/DazToUnreal.cpp` `Tick()` | after JSON parse, `if (FDazSceneImporter::IsSceneManifest(JsonObject)) { FDazSceneImporter::Import(JsonObject, FileName); } else { ImportFromDaz(...) }` | ~5 lines |
| `UnrealPlugin/.../DazToUnreal.Build.cs` | add any extra modules the level instance code needs (to be confirmed by the M6 spike) | 1-3 lines |

Because the routing is decided by a manifest-only key (`"ManifestType": "DazSceneShot"`), a `.dtu` can never take the new path and a manifest can never take the old one.

---

## 3. How the Daz side works

### 3.1 Toolbar button, toggle, shot prompt (M1)

- `DzUnrealSceneAction` shows as **"Daz to Unreal: Export Scene"** in `File/Send To`. Daz actions can be dragged onto any toolbar via *Window → Workspace → Customize*. I'll also return a default toolbar from the action if the 4.24 SDK supports it on `DzAction` (risk R9).
- If `SceneExportEnabled` is off, the action shows a one-line message ("Scene export is turned off in Daz to Unreal → Advanced Settings") and exits.
- The shot dialog has:
  - **Scene name.** Defaults to the saved `.duf` file name, otherwise the last scene name used. Required.
  - **Shot name.** Suggested by section 3.5.
  - Read-only lines for the intermediate folder and port, taken from the bridge settings.
- After confirmation the exporter writes to `<IntermediatePath>/SceneExports/<Scene>/<Shot>/`, clearing that folder first. Shared prop FBX files go to `<IntermediatePath>/SceneExports/_Props/<Product>/<Label>.fbx`.

### 3.2 Scene walker (M1, finished in M2)

The walker visits every top-level node of `dzScene` recursively and classifies each node:

| Node | Path |
| --- | --- |
| `DzLight`, `DzCamera` | skipped (and so are their children) |
| `DzBone` | skipped as a node (its figure handles it) |
| hidden (`!isVisible()`) | skipped, see question 8.4 |
| `DzGroupNode`, null nodes | no mesh; children are still walked (they carry their own world transforms) |
| `DzInstanceNode` | placement of its target's mesh(es), see 3.4 |
| `DzFigure` that follows another figure | an *item* of its root figure: wardrobe, hair, or geograft (`isGraftFollowing()`) |
| `DzFigure` with presentation type `Actor*`, or with followers | **figure path** (baked) |
| any other node with geometry | **prop path**, unless it is "modified" (next point) |

A prop counts as **modified**, and moves to the figure path, when any morph modifier on its object has a non-default value or any of its bones has a non-default local transform. That matches spec decision 2 ("a couch with a cushion morph dialed in"). The exact rule is question 8.3.

### 3.3 Mesh export (M2)

Every mesh is written by **our own static FBX writer** from the node's cached geometry (`DzObject::getCachedGeom()`), which is already posed and morphed and at the viewport subdivision level. Material slots in the FBX are named after the surface names (`<CleanLabel>_<Surface>`, matching the bridge's convention), so `GetBaseMaterial()`'s suffix checks (`_Face`, `_Torso`, `_scalp`…) keep working. UVs (including UDIM tiles, left uncollapsed) and normals are written.

- **Props:** the cached world-space vertices are multiplied by the inverse of the node's world transform, giving local space. The placement gets the node's world transform. Each product + clean label is exported once per shot. If two nodes with the same key have different surface values, the first wins and the others get a warning in the manifest. Unreal shows it.
- **Figures:** body, each wardrobe item, hair, and each geograft are written as separate FBX files in world space: `<Label>_Body.fbx`, `<Label>_<ItemLabel>.fbx`. Their placement transform is identity.
- **Instanced figures:** a figure that is the target of a node instance is baked once in the figure root's local space (inverse of the root's world transform), and every placement (the original and each instance) gets its own world transform.
- Fallback if cached geometry proves unreliable in 4.24 (risk R1): Daz's own `DzObjExporter` per node, which already writes what the viewport shows. Unreal imports OBJ natively.

### 3.4 Node instances (M2)

- An instance never exports geometry. It adds a placement pointing at its target's mesh with the instance's own world transform, and uses the target's material set (spec decision 11).
- If the target is a prop, the prop path is used as normal. If the target is a figure, it gets the local-space bake from 3.3.
- Instances whose target is a group, or that include the target's children: each child mesh is placed with `instanceWS × inverse(targetWS) × childWS`. This needs testing in Daz (risk R3).

### 3.5 Naming helpers (M1/M2)

- **Clean label:** strip one trailing ` (<digits>)` with the regex `\s\(\d+\)$`. `couch02 (2)` becomes `couch02`. `couch02` stays `couch02`, and so does `Couch (Red)`, because the suffix has to be digits.
- **Product:** the product name from the Daz content database (`DzAssetMgr` lookup by the node's asset file; exact call to confirm against the 4.24 SDK, risk R2). If nothing is found, the product is `Custom` and the subfolder is the content-relative folder of the node's asset file, for example `Custom/People/Genesis 8 Male/`. For figures, see question 8.2.
- **Next shot name:** scan the scene's export folder and the last name used for `<prefix><number>`, then suggest the highest number + 1 with the same zero padding (`Shot01` → `Shot02`, `Shot09` → `Shot10`, `CloseUp3` → `CloseUp4`). With no history the suggestion is `Shot01`.

### 3.6 Materials and textures in the manifest (M2)

The bridge's `startMaterialBlock` / `writeMaterialProperty` are reused, so every property keeps the exact name, label, type and value the Unreal side already maps. Each texture is recorded once in a `Textures` table with:

- its absolute source path;
- its **runtime-relative path** (`dzApp->getContentMgr()->getRelativePath(path, true)`, for example `Runtime/Textures/DAZ/...`), or for files outside any content directory, `Custom/<last two folders>/<file>`;
- size and modified time.

Temporary textures (layered images) are copied by `exportAssetWithDtu` and named by MD5 (`Generated/<md5>.png`), so the same content always gets the same name.

---

## 4. Shot manifest format (JSON)

File: `<IntermediatePath>/SceneExports/<Scene>/<Shot>/<Scene>_<Shot>.dzshot.json`. It is sent to Unreal over the same UDP port as a `.dtu`.

Conventions:

- All paths use forward slashes.
- Transforms are in **Daz space** (centimeters, Y-up, right-handed) as a 4×4 row-major world matrix, plus decomposed translation, rotation (quaternion x, y, z, w) and scale for readability. Unreal converts the matrix with one basis-change function that matches what its FBX importer does to the mesh, so mesh and placement can't drift apart (see M6 test).
- `Key` fields are the identity rules from the spec, spelled out: material set `Product|CleanLabel`, prop mesh `Product|CleanLabel`, baked mesh `Scene|Shot|CleanLabel|Item`.
- Unknown fields are ignored by Unreal, and `ManifestVersion` is checked, so the format can grow.

### Worked example

Scene `LivingRoom`, shot `Shot02`, containing:

- posed **Michael8** (Genesis 8 Male) wearing a T-shirt, with hair and a geograft;
- **couch02** from "Urban Loft" placed twice (the second is labeled `couch02 (2)` by Daz);
- a **floorlamp01** prop plus a Daz **node instance** of it.

```json
{
  "ManifestType": "DazSceneShot",
  "ManifestVersion": 1,
  "ExporterVersion": "2023.1.0.53-scene1",
  "SceneName": "LivingRoom",
  "ShotName": "Shot02",
  "ExportedAt": "2026-10-17T14:03:11Z",
  "DazUnits": "cm",
  "DazUpAxis": "Y",
  "ExportFolder": "C:/Users/Me/Documents/DazToUnreal/SceneExports/LivingRoom/Shot02",
  "PropFolder": "C:/Users/Me/Documents/DazToUnreal/SceneExports/_Props",

  "Textures": [
    { "Id": "T0", "Source": "D:/DazLib/Runtime/Textures/DAZ/Characters/Genesis8/Michael8/Michael8_Face_D_1001.jpg",
      "RuntimePath": "Runtime/Textures/DAZ/Characters/Genesis8/Michael8/Michael8_Face_D_1001.jpg",
      "Size": 4194877, "Modified": "2019-06-04T10:22:51Z" },
    { "Id": "T1", "Source": "D:/DazLib/Runtime/Textures/Vendor/Classic Tee/ClassicTee_Diffuse.jpg",
      "RuntimePath": "Runtime/Textures/Vendor/Classic Tee/ClassicTee_Diffuse.jpg",
      "Size": 2201113, "Modified": "2021-02-11T08:00:00Z" },
    { "Id": "T2", "Source": "D:/DazLib/Runtime/Textures/Vendor/Short Hair/ShortHair_Trans.png",
      "RuntimePath": "Runtime/Textures/Vendor/Short Hair/ShortHair_Trans.png",
      "Size": 1048890, "Modified": "2020-09-30T17:41:02Z" },
    { "Id": "T3", "Source": "D:/DazLib/Runtime/Textures/Vendor/Urban Loft/couch02_fabric_D.jpg",
      "RuntimePath": "Runtime/Textures/Vendor/Urban Loft/couch02_fabric_D.jpg",
      "Size": 3120001, "Modified": "2022-01-05T12:00:00Z" },
    { "Id": "T4", "Source": "D:/DazLib/Runtime/Textures/Vendor/Urban Loft/floorlamp01_D.jpg",
      "RuntimePath": "Runtime/Textures/Vendor/Urban Loft/floorlamp01_D.jpg",
      "Size": 900345, "Modified": "2022-01-05T12:00:00Z" }
  ],

  "MaterialSets": [
    { "Key": "Genesis 8 Male|Michael8", "Product": "Genesis 8 Male", "ProductSource": "BaseFigure",
      "Label": "Michael8", "ContentType": "Actor/Character",
      "Surfaces": [
        { "Surface": "Face", "MaterialName": "Michael8_Face", "ShaderType": "Iray Uber",
          "Properties": [
            { "Name": "Asset Type", "Label": "Asset Type", "Value": "Actor/Character", "Data Type": "String", "Texture": "" },
            { "Name": "Diffuse Color", "Label": "Base Color", "Value": "#ffffff", "Data Type": "Color", "Texture": "T0" },
            { "Name": "Glossy Roughness", "Label": "Glossy Roughness", "Value": 0.42, "Data Type": "Double", "Texture": "" }
          ] }
      ] },
    { "Key": "Genesis 8 Male|Michael8 Graft", "Product": "Genesis 8 Male", "ProductSource": "BaseFigure",
      "Label": "Michael8 Graft", "ContentType": "Follower/Attachment/Lower-Body/Hip/Front",
      "Surfaces": [ { "Surface": "Genitalia", "MaterialName": "Michael8Graft_Genitalia", "ShaderType": "Iray Uber", "Properties": [ ] } ] },
    { "Key": "Classic Tee for Genesis 8 Male|ClassicTee", "Product": "Classic Tee for Genesis 8 Male", "ProductSource": "ContentDB",
      "Label": "ClassicTee", "ContentType": "Follower/Wardrobe/Shirt",
      "Surfaces": [ { "Surface": "Shirt", "MaterialName": "ClassicTee_Shirt", "ShaderType": "Iray Uber",
        "Properties": [ { "Name": "Diffuse Color", "Label": "Base Color", "Value": "#d0d0d0", "Data Type": "Color", "Texture": "T1" } ] } ] },
    { "Key": "Short Hair for Genesis 8 Male|ShortHair", "Product": "Short Hair for Genesis 8 Male", "ProductSource": "ContentDB",
      "Label": "ShortHair", "ContentType": "Follower/Hair",
      "Surfaces": [ { "Surface": "Hair", "MaterialName": "ShortHair_Hair", "ShaderType": "Iray Uber",
        "Properties": [ { "Name": "Cutout Opacity", "Label": "Cutout Opacity", "Value": 1.0, "Data Type": "Double", "Texture": "T2" } ] } ] },
    { "Key": "Urban Loft|couch02", "Product": "Urban Loft", "ProductSource": "ContentDB",
      "Label": "couch02", "ContentType": "Prop",
      "Surfaces": [ { "Surface": "Fabric", "MaterialName": "couch02_Fabric", "ShaderType": "Iray Uber",
        "Properties": [ { "Name": "Diffuse Color", "Label": "Base Color", "Value": "#ffffff", "Data Type": "Color", "Texture": "T3" } ] } ] },
    { "Key": "Urban Loft|floorlamp01", "Product": "Urban Loft", "ProductSource": "ContentDB",
      "Label": "floorlamp01", "ContentType": "Prop",
      "Surfaces": [ { "Surface": "Lamp", "MaterialName": "floorlamp01_Lamp", "ShaderType": "Iray Uber",
        "Properties": [ { "Name": "Diffuse Color", "Label": "Base Color", "Value": "#ffffff", "Data Type": "Color", "Texture": "T4" } ] } ] }
  ],

  "Meshes": [
    { "Id": "M0", "Kind": "BakedFigure", "Key": "LivingRoom|Shot02|Michael8|Body", "Label": "Michael8", "Item": "Body",
      "File": "Michael8_Body.fbx", "Space": "World", "SubdivisionLevel": 1, "MaterialSet": "Genesis 8 Male|Michael8" },
    { "Id": "M1", "Kind": "BakedFigure", "Key": "LivingRoom|Shot02|Michael8|Michael8Graft", "Label": "Michael8", "Item": "Michael8Graft",
      "File": "Michael8_Michael8Graft.fbx", "Space": "World", "SubdivisionLevel": 1, "MaterialSet": "Genesis 8 Male|Michael8 Graft" },
    { "Id": "M2", "Kind": "BakedFigure", "Key": "LivingRoom|Shot02|Michael8|ClassicTee", "Label": "Michael8", "Item": "ClassicTee",
      "File": "Michael8_ClassicTee.fbx", "Space": "World", "SubdivisionLevel": 1, "MaterialSet": "Classic Tee for Genesis 8 Male|ClassicTee" },
    { "Id": "M3", "Kind": "BakedFigure", "Key": "LivingRoom|Shot02|Michael8|ShortHair", "Label": "Michael8", "Item": "ShortHair",
      "File": "Michael8_ShortHair.fbx", "Space": "World", "SubdivisionLevel": 0, "MaterialSet": "Short Hair for Genesis 8 Male|ShortHair" },
    { "Id": "M4", "Kind": "Prop", "Key": "Urban Loft|couch02", "Product": "Urban Loft", "Label": "couch02",
      "File": "../../_Props/Urban_Loft/couch02.fbx", "FileMD5": "9b1c…", "Space": "Local", "MaterialSet": "Urban Loft|couch02" },
    { "Id": "M5", "Kind": "Prop", "Key": "Urban Loft|floorlamp01", "Product": "Urban Loft", "Label": "floorlamp01",
      "File": "../../_Props/Urban_Loft/floorlamp01.fbx", "FileMD5": "3fa0…", "Space": "Local", "MaterialSet": "Urban Loft|floorlamp01" }
  ],

  "Placements": [
    { "Mesh": "M0", "Node": "Michael8", "Source": "Figure", "Matrix": [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1] },
    { "Mesh": "M1", "Node": "Michael8 Graft", "Source": "Figure", "Matrix": [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1] },
    { "Mesh": "M2", "Node": "ClassicTee", "Source": "Figure", "Matrix": [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1] },
    { "Mesh": "M3", "Node": "ShortHair", "Source": "Figure", "Matrix": [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1] },

    { "Mesh": "M4", "Node": "couch02", "Source": "Prop",
      "Matrix": [1,0,0,0, 0,1,0,0, 0,0,1,0, -120.0,0,35.5,1],
      "Translation": [-120.0, 0.0, 35.5], "Rotation": [0,0,0,1], "Scale": [1,1,1] },
    { "Mesh": "M4", "Node": "couch02 (2)", "Source": "Prop",
      "Matrix": [0,0,-1,0, 0,1,0,0, 1,0,0,0, 210.0,0,-40.0,1],
      "Translation": [210.0, 0.0, -40.0], "Rotation": [0,0.7071068,0,0.7071068], "Scale": [1,1,1] },

    { "Mesh": "M5", "Node": "floorlamp01", "Source": "Prop",
      "Matrix": [1,0,0,0, 0,1,0,0, 0,0,1,0, -60.0,0,-80.0,1],
      "Translation": [-60.0, 0.0, -80.0], "Rotation": [0,0,0,1], "Scale": [1,1,1] },
    { "Mesh": "M5", "Node": "floorlamp01 Instance", "Source": "Instance", "InstanceOf": "floorlamp01",
      "Matrix": [1.2,0,0,0, 0,1.2,0,0, 0,0,1.2,0, 300.0,0,90.0,1],
      "Translation": [300.0, 0.0, 90.0], "Rotation": [0,0,0,1], "Scale": [1.2,1.2,1.2] }
  ],

  "Warnings": []
}
```

Unreal turns this into:

```
/Game/DazTextures/Runtime/Textures/DAZ/Characters/Genesis8/Michael8/Michael8_Face_D_1001
/Game/DazTextures/Runtime/Textures/Vendor/Urban_Loft/couch02_fabric_D …
/Game/DazMaterials/Genesis_8_Male/Michael8/Michael8_Face
/Game/DazMaterials/Urban_Loft/couch02/couch02_Fabric
/Game/DazProps/Urban_Loft/couch02            (static mesh, local space, placed twice)
/Game/DazProps/Urban_Loft/floorlamp01        (placed twice: prop + instance)
/Game/DazScenes/LivingRoom/Shot02/Michael8_Body, Michael8_ClassicTee, Michael8_ShortHair, Michael8_Michael8Graft
/Game/DazScenes/LivingRoom/LivingRoom_Shot02 (level used as a level instance)
```

(Unreal does not allow spaces in package paths, so `Genesis 8 Male` becomes `Genesis_8_Male` in the Content Browser. That is the deterministic rule from section 5.1.)

---

## 5. How the Unreal side works

### 5.1 Path sanitizing (M3)

One function, `FDazScenePaths::ToPackagePath(root, relativePath)`, used for every library path:

1. Split on `/`. For each segment, replace every character outside `[A-Za-z0-9_-]` with `_`, collapse runs of `_`, and trim `_` from both ends. An empty segment becomes `_`.
2. Drop the file extension from the asset name. If two different source files in the same folder end up with the same name (`foo.jpg` and `foo.png`), the later one gets `_png` appended. The ledger remembers the decision, so it's stable from then on.
3. If the resulting `/Game/…` object path is longer than **150 characters**, segments are shortened from the middle outwards and an 8-character hash of the *original* runtime path is appended to the asset name. 150 leaves room for a typical project folder under Windows' 260-character limit; it can be changed in settings.

Same input, same output, every time.

### 5.2 Texture sync (M3)

- The ledger is `Saved/DazSceneExporter/TextureLedger.json`: Unreal path → {source path, size, modified time}.
- For each manifest texture, compute the target path. If the asset exists *and* the ledger has the same size and modified time, skip it. Otherwise import (or reimport over the existing asset; the runtime folder wins) straight from the source file with `UAssetImportTask` (`bReplaceExisting = true`, automated). No copy into the intermediate folder.
- Apply the same sRGB fix-up for cutout and opacity maps the bridge uses, after import (deferred one tick, as the bridge does).
- Nothing is ever deleted from `DazTextures`.

### 5.3 Materials (M4)

- A material set lives at `/Game/DazMaterials/<Product>/<CleanLabel>/`, with one instance per surface named `<CleanLabel>_<Surface>`.
- For each surface, the manifest properties are parsed into `FDUFTextureProperty` the same way `ImportFromDaz` does for Version 4 blocks. That's a straight copy of the ~80-line parsing loop into the new file, so the original isn't touched. Texture property values are full `/Game/DazTextures/...` object paths. Then `FDazToUnrealMaterials::CreateMaterial()` builds it from the user's Material Packs and master material settings.
- **Update in place:** if the instance already exists, it is built into a transient package first and its parameters (including static switches) are copied onto the existing asset with `CopyMaterialUniformParametersEditorOnly`. Meshes in earlier shots keep pointing at the same asset and pick up the change.
- **Reporting:** parameters are compared before and after. Each changed set gets one Output Log line (`LogDazSceneExporter: Updated material set Genesis_8_Male/Michael8 (3 surfaces changed: Face, Torso, Arms)`) and one editor toast summarizing them.

### 5.4 Mesh import (M5)

- Static-mesh FBX import through `UAssetImportTask` + `UFbxFactory`: `bImportMaterials = false`, `bImportTextures = false`, `bCombineMeshes = true`, normals imported, `bConvertScene = true`, `bForceFrontXAxis = false`.
- After import, each material slot is matched by name to `<CleanLabel>_<Surface>` in the mesh's material set.
- **Props** go to `/Game/DazProps/<Product>/<CleanLabel>`. If the asset exists and its stored source-file MD5 equals the manifest's `FileMD5`, the import is skipped. Props are never deleted by a shot.
- **Baked figures** go to `/Game/DazScenes/<Scene>/<Shot>/<CleanLabel>_<Item>`.

### 5.5 Level instance and re-export (M6)

- On every import of a shot:
  1. **Clear the shot:** delete every asset in `/Game/DazScenes/<Scene>/<Shot>/` (baked figures only), and every actor in the shot level. Textures, materials and props are never touched.
  2. Import, then repopulate the level `/Game/DazScenes/<Scene>/<Scene>_<Shot>` with one `AStaticMeshActor` per placement (static mobility, actor label = Daz node label, tag `DazSceneExporter`), then save it.
- The first time a shot is created, an `ALevelInstance` actor pointing at that level is also placed in the currently open level (question 8.6). On re-export, existing level instance actors reload automatically because the level asset path doesn't change.
- How the level is edited without opening it is the main technical unknown (risk R4). M6 starts with a small spike to pick between (a) loading the level package as an editor world and spawning into it directly, and (b) building the actors in a temporary level and using `ULevelInstanceSubsystem::CreateLevelInstanceFrom`.

---

## 6. Milestones (adjusted)

Changes from your list: the Unreal-side "receive and log" stub moves into M1, so the UDP handoff and the "doesn't touch the asset path" guarantee are tested from day one. M2 is split in two because props and figure baking are separately testable and are the riskiest Daz work.

| # | Milestone | Daz side | Unreal side |
| --- | --- | --- | --- |
| **1** | Scaffolding | Toolbar action, Advanced Settings toggle, shot dialog with next-name suggestion, scene walker (classification only), manifest with `Placements` for every non-skipped node but **no meshes**, UDP send | Settings page with toggle; `Tick` hook; parse and validate manifest; log a summary ("Shot LivingRoom_Shot02: 4 figure items, 3 props, 1 instance") and nothing else |
| **2a** | Props + materials | Static FBX writer; props in local space, deduplicated by product + clean label; product lookup + Custom fallback; `MaterialSets` and `Textures` in manifest | — (manifest inspected by hand) |
| **2b** | Figures + instances | World-space bake per item (body, wardrobe, hair, geografts) at viewport SubD; modified-prop detection; node instances, including instanced figures in local space | — |
| **3** | Texture sync | — | Path rule, ledger, import/skip/overwrite, sRGB fix-up |
| **4** | Materials | — | Material sets, in-place update, change reporting (log + toast) |
| **5** | Mesh import | — | Props to `DazProps` with MD5 skip, figures to shot folder, slot → material assignment |
| **6** | Level instance | — | Spike, then create/repopulate `Scene_Shot`, placements incl. repeats and instances, shot clearing |
| **7** | Hardening | Odd labels and long paths, missing-product fallback | Re-export test, Custom fallback, long paths, final regression pass of the standard asset export |

Each milestone ends with: your compile results for both plugins, a summary of what changed in plain terms, and an in-app test checklist.

**Regression guard for the standard asset export (every milestone):** the diff to existing files stays limited to the hooks in section 2. The checklist always includes one character send (skeletal mesh with morphs), one prop send (static mesh) and one Environment send through the normal bridge dialog, with a comparison against a pre-fork import.

---

## 7. Risks and hard or ambiguous spots

| # | Risk | Plan |
| --- | --- | --- |
| R1 | **Cached geometry level.** I'm relying on `getCachedGeom()` returning the viewport-SubD, posed, morphed mesh in world space for both figures and props. That's how the viewport draws, but it needs checking on 4.24 (HD morphs, SubD levels, UDIM UVs). | First thing tested in M2a/M2b. Fallback is the OBJ exporter per node (3.3). |
| R2 | **Product lookup API.** The bridge never queries the content database, so there's no in-repo example. It will go through `DzAssetMgr` (asset by file → product name), but the exact 4.24 call has to be confirmed against your SDK headers. | Isolated in `DzSceneNaming`. Custom fallback covers any failure. |
| R3 | **Instance semantics.** Daz node instances can copy the target's children and have several modes. The transform composition for child meshes (3.4) needs checking in Daz. | Simple instances first, groups second; both in the M2b checklist. |
| R4 | **Editing a level asset that isn't open**, with World Partition / one-file-per-actor in 5.8 (external actor packages must be deleted on clear). | M6 spike, two candidate approaches (5.5). |
| R5 | **Axis and transform conversion.** Mesh (converted by the FBX importer) and placements (converted by our code) have to agree. The Environment importer's Euler mapping is a known source of subtle rotation bugs. | Placements carry full matrices; one conversion function; the M1/M6 checklist uses an asymmetric, rotated, non-uniformly scaled test prop. |
| R6 | **Material parity with the bridge.** The bridge's "intermediate parent material" grouping and per-character subsurface profiles are tied to one character import. Scene export creates instances directly from the master materials, so skin may look slightly different from a bridge import until subsurface profiles are added per set. | Accept for v1, compare side by side in M4, add per-set subsurface profiles if needed. |
| R7 | **Same key, different look.** Two `couch02` copies with different materials in Daz map to one material set (by design). The last write wins, so it's surfaced as a warning. Re-texturing one copy needs a new label (e.g. `couch02_red`), as the spec shows. | Warning in manifest and Unreal log. |
| R8 | **Geografts as separate meshes** (spec decision 3) can show a seam at the weld line, because each mesh is subdivided and normal-smoothed separately. | Test in M2b. If visible, an option to merge grafts into the body mesh (still one body material set). |
| R9 | **Default toolbar placement** of a plugin action depends on SDK support in `DzAction`. | Worst case you place it once via *Customize*; Daz remembers it. |
| R10 | **Qt 4 / DS 4.24 vs DS6.** The repo also builds a DS6 / Qt6 variant (`DAZ6`). New Daz code will compile under both where cheap, but only DS 4.24 is tested. | — |
| R11 | **UDP message size.** One datagram, 2048-byte buffer. Fine for a path, but very long intermediate folders plus scene and shot names must stay below it. | Validated before sending. |
| R12 | **Version labelling.** Your prompt says 5.8.0.731. The repo shows plugin `VersionName` 5.8.0 and Daz plugin 2023.1.0.53. I'm treating current `HEAD` (`9192689`) as the base. | Question 8.7. |

---

## 8. Questions for you

1. **Compiling.** I can't build either plugin in this environment (no Daz SDK, FBX SDK or UE 5.8.1). OK for you to compile at each milestone and paste errors back to me?
2. **"Product" for characters.** In the content database, Genesis 8 Male's base belongs to *Genesis 8 Starter Essentials*, not to a product called *Genesis 8 Male*. The spec puts Michael8, Michael8_Sunburned and Dain8 in a "Genesis 8 Male" folder. My proposal: for `Actor` figures, use the **base figure's name** (Genesis 8 Male, Genesis 9…) as the product; for everything else (wardrobe, hair, props), use the content database product. OK?
3. **Prop vs. figure rule.** Proposal: a node takes the figure path if it is an `Actor*` figure or has followers, or if any morph is dialed away from its default or any bone is posed. Everything else with geometry is a reusable prop. Rigged props at rest (a door at 0°) stay props. OK?
4. **Hidden nodes.** Skip nodes hidden in the viewport, and their children? (Proposed: yes.)
5. **Spec decision 8, product-name prefix on prop labels.** I read this as *your* labeling convention in Daz, not something the exporter adds. The exporter uses the label as-is, minus the ` (#)` suffix. Correct?
6. **Level instance placement.** Besides creating `Scene_Shot`, should the first import also drop a Level Instance actor into whatever level is open? (Proposed: yes, first import only, at the origin.)
7. **Base version.** Confirm current `HEAD` is the 5.8.0.731 base you mean.
8. **Height-to-normal generation.** Leave the bridge's optional "generate normal maps from height maps" off for scene export at first? It creates temporary files and slows export. (Proposed: off.)

Once these are settled I'll start Milestone 1.
