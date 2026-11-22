# Release v0.2.0 — Symptom Report

## Status

Planning complete — implementation pending.

## Goal

Add a second top-level workspace tab for reporting physical, medical,
psychiatric, neurological and other symptoms experienced in the last 24 hours.
The existing CellScope window remains intact inside the first tab.

## Planned features

| Feature | Description | Completed commit |
|---|---|---|
| `feature/tabbed-window-shell` | Add RF Check / Symptom Report tabs and move the complete current window into RF Check; place global Export/Upload/Settings on the right of the tab bar. | `2cd520a` |
| `feature/dataset-lifecycle-v2` | Start a dataset at app launch, gate RF writes by capture state, stream symptom data, and rotate to a new dataset when an upload is queued. | `db19e94` |
| `feature/symptom-body-map` | Temporary 2D front/back fallback until 3D is ready; retire it after 3D works. | `e4ec3de` |
| `feature/anatomy-assets` | Download versioned 3D assets from a separate B2 bucket into the local cache, with SHA-512 verification, progress UI and offline fallback. | `734ab32` |
| `feature/anatomy-b2-presign` | Add a Worker route that signs short-lived GETs for the private anatomy B2 bucket so the read-only key never enters the app. | |
| `feature/anatomy-renderer` | OpenGL 2.1 3D viewport with orbit/zoom, textured model, male/female side-by-side frontal display, and adaptive LOD selection. | |
| `feature/anatomy-picking` | Muscle code ID-atlas picking, precise 3D surface points, and derived code-to-body-region mapping for review. | |
| `feature/anatomy-integration` | Replace the 2D map with the 3D viewport and connect picked codes/regions to symptom cards. | |
| `feature/symptom-cards` | Add scrollable symptom cards, multi-symptom subsections, fuzzy search, focus-by-circle and card removal. | |
| `feature/symptom-catalog` | Ship a provisional local symptom catalog and schema-driven controls for fuzzy symptom selection. | |
| `feature/symptom-report-storage` | Stream one report per dataset to JSON, include it in archives/manifests, and support report-only datasets. | |

## Window and tabs

- Outer top bar:
  - left: `RF Check`, `Symptom Report`
  - right: `Export`, `Upload`, `Settings`
- The complete current window lives inside RF Check:
  - logo and status/header
  - cell samples
  - webcam
  - readings
  - footer
  - RF-specific modals
- The three global actions are outside both tabs and apply to the current dataset.
- RF modals stay inside RF Check and hide when the tab is left.
- Symptom Report modals stay inside Symptom Report.
- Switching tabs does not reset RF capture, readings, camera or report state.

## Dataset lifecycle

- A dataset folder starts when the interactive application starts.
- RF capture writes into the active dataset only while capture is running.
- Pausing stops RF writes without starting a new dataset; resuming continues the same dataset.
- Symptom report changes are written immediately and atomically.
- When Upload creates and queues the archive, a new dataset starts immediately.
- The new dataset starts with an empty symptom report.
- Capture may continue into the new dataset after rotation.
- A dataset containing only symptom reports is valid and uploadable.
- The dataset archive and manifest include `symptom-report.json`.

## Symptom Report tab

### 3D anatomical model

- Replace the temporary 2D body map with the purchased male/female anatomical model.
- Show both figures side by side, frontal, for the user to choose.
- Fully rotate, zoom and pan the selected model in 3D.
- Click a muscle region to add a symptom card; store the precise surface point.
- Use the model's painted muscle codes as the primary mapping.
- Build a code ID atlas in the model UV layout; click sampling returns the code.
- Derive a code-to-body-region table and present it for user review; manually
  correct any uncertain mappings.
- No front/back selection model: the 3D surface point and rotation replace it.
- Selected regions highlight on the model.
- Male/female selection is recorded with the cards.
- The 2D map remains only as a temporary fallback and is removed once 3D works.

### 3D rendering and assets

- Renderer: OpenGL 2.1 compatibility, proven through the SDL_Renderer/OpenGL spike.
- Asset source: separate B2 bucket, not Git or the application release archive.
- Download automatically when Symptom Report opens, with a progress bar.
- Cache at `~/.cellscope/assets/anatomy/<version>/`.
- Verify SHA-512 before use; show an error/retry state if unavailable.
- Male/female variants, 4K diffuse texture, and a muscle-code ID atlas.
- Generate low/medium/high LODs while preserving UVs and code mapping.
- Select LOD adaptively from measured frame time and available hardware.
- Keep lighting simple: diffuse texture, ambient and directional light, mipmaps,
  sRGB and anisotropic filtering.

### Cards and symptom subsections

- Each card contains one or more symptom subsections.
- A subsection starts with a fuzzy-search input, completion list and keyboard
  selection.
- Selecting a symptom loads the controls defined for that symptom.
- `Add another symptom` adds a subsection below the existing ones.
- Cards can be minimized and expanded vertically to fit their controls.
- Card fields (provisional until the taxonomy is finalized):
  - onset within the last 24 hours
  - duration
  - severity
  - frequency/pattern
  - description
  - triggers
  - relief
  - notes

## Provisional symptom catalog

The first catalog is intentionally provisional and lives in a data file so the
final taxonomy can replace or extend it without UI changes. Initial entries:

- pain
- numbness
- tingling / pins and needles
- weakness
- burning
- pressure
- swelling
- stiffness
- tremor
- headache
- dizziness / lightheadedness
- vision change
- hearing change
- nausea
- fatigue
- anxiety
- confusion
- other

Each definition carries a label, aliases, category and a list of controls.
Supported control types:

- text
- multiline text
- number
- time
- single-select
- multi-select
- checkbox
- severity scale
- label / informational image

## Report JSON

One report per dataset, written atomically as:

```text
<dataset>/symptom-report.json
```

Top-level shape:

```json
{
  "schema": "cellscope-symptom/1",
  "dataset": "2026-11-22T...",
  "window_hours": 24,
  "updated_at_unix_ms": 0,
  "cards": []
}
```

Each card records its kind (`region`, `whole_body`, `non_physical`), selected
regions with view and point, displayed marker, minimized state, and symptom
subsections with their selected definition and control values.

## Acceptance criteria

- The complete existing RF window is inside RF Check; only the tab/action bar is outside.
- Export, Upload and Settings are on the right side of the tab bar.
- RF and symptom modals are scoped to their tabs and hidden on switch.
- A dataset exists immediately at interactive app launch.
- RF writes start/stop with capture without rotating the dataset.
- Symptom edits are visible in JSON immediately.
- Uploading queues the archive and starts a new dataset immediately.
- Front and back maps expose 27 and 28 regions and all 55 are selectable.
- Selecting the same region repeatedly adds cards and non-overlapping circles.
- Circle focus and card removal behave as specified.
- Whole-body and non-physical buttons add cards without deleting existing cards.
- Fuzzy symptom search loads the correct controls and supports multiple subsections.
- One report per dataset round-trips through JSON and is included in archive/manifest.
- A report-only dataset can be exported and uploaded.
- The 3D model loads from B2/cache with progress, checksum verification and retry.
- Both male and female models appear frontally side by side; the user can choose one.
- The chosen model rotates and zooms in 3D.
- Clicking a muscle returns its painted code and creates a symptom card with a 3D point.
- The derived code-to-region table is reviewed and corrected before release.
- Adaptive LOD keeps interaction responsive on modest hardware.
- The 2D fallback is removed after 3D parity is verified.
- UI regression covers both tabs, 3D model states and a populated report.

## Out of scope

- Final medical/psychiatric/neurological symptom taxonomy and per-symptom controls.
- RF data capture from the symptom tab.
- Remote report editing or separate report upload.
- Changes to the RF measurement pipeline.
- Shipping the purchased model files in Git or release archives.
- Vulkan/Metal/D3D12 rendering or an SDL_GPU UI migration in this release.

## Release checklist

- [x] Feature scope agreed
- [ ] Feature branches merged into `develop`
- [ ] `release/0.2.0` pushed and all platform builds green
- [ ] GitHub release `v0.2.0` generated from the release branch
- [ ] Release branch fast-forwarded into `main`
- [ ] `main` pushed and release assets refreshed
- [ ] `main` merged back into `develop`
