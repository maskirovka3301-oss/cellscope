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
| `feature/tabbed-window-shell` | Add RF Check / Symptom Report tabs and move the complete current window into RF Check; place global Export/Upload/Settings on the right of the tab bar. | |
| `feature/dataset-lifecycle-v2` | Start a dataset at app launch, gate RF writes by capture state, stream symptom data, and rotate to a new dataset when an upload is queued. | |
| `feature/symptom-body-map` | Add neutral front/back anatomical body maps with 55 view-specific regions, precise points, non-overlapping circles and zoom. | |
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

### Body map

- Body fixed on the left; scrollable cards on the right.
- Original, neutral front and back illustrations:
  - no sexual organs
  - not identifiable as male or female
  - selected regions shown in red
- Front: 27 selectable regions.
- Back: 28 selectable regions.
- Front and back selections are distinct: 55 view-specific selections total.
- Shared anatomical areas use the same region name but different view entries.
- Region list:
  - head
  - neck
  - left/right shoulder
  - left/right chest (front) / left/right upper back (back)
  - left/right abdomen (front) / left/right lower back (back)
  - groin (front) / left/right buttocks (back)
  - left/right upper leg
  - left/right lower leg
  - left/right knee
  - left/right ankle
  - left/right foot
  - left/right arm
  - left/right elbow
  - left/right wrist
  - left/right hand

### Interaction

- Single click a region adds a new card for that region/view.
- Double click zooms into the region.
- In zoom, click to set an optional precise point; clicking again moves it.
- Small close button at the upper-right of the body view exits zoom.
- Each region card gets a small circle at its point.
- If a circle would overlap, use the nearest free position and draw a leader line
  to the precise point.
- Store the precise point and displayed marker separately.
- Clicking a circle focuses and scrolls to its card.
- Removing a card removes its circle.
- Multiple cards may target the same region/view.
- Circles never overlap.
- Top-left body controls:
  - `add whole body symptom` creates one card covering all 55 selections.
  - `add non-physical symptom` creates a card with no body region.
- Whole-body, non-physical and region cards may coexist.
- All cards are preserved unless explicitly removed.

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
- UI regression covers both tabs, body views, zoom, circles and a populated report.

## Out of scope

- Final medical/psychiatric/neurological symptom taxonomy and per-symptom controls.
- RF data capture from the symptom tab.
- Remote report editing or separate report upload.
- Changes to the RF measurement pipeline.

## Release checklist

- [x] Feature scope agreed
- [ ] Feature branches merged into `develop`
- [ ] `release/0.2.0` pushed and all platform builds green
- [ ] GitHub release `v0.2.0` generated from the release branch
- [ ] Release branch fast-forwarded into `main`
- [ ] `main` pushed and release assets refreshed
- [ ] `main` merged back into `develop`
