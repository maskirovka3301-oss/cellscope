# Symptom report storage

Each dataset streams one `symptom-report.json` into its dataset folder. The
file is rewritten atomically (`.tmp` + rename) whenever card content changes
and on session begin/end/export.

```json
{
  "schema": "cellscope-symptom-report/1",
  "dataset": "20261125_130600",
  "created_at_unix_ms": 1795608360213,
  "cards": [
    {
      "kind": "muscle",
      "code": "VL",
      "region": "Thigh",
      "side": "female",
      "point": {"x": -0.428, "y": -0.134, "z": 0.172},
      "minimized": false,
      "symptoms": [
        {
          "name": "Pain",
          "controls": 255,
          "severity": 6,
          "onset_hours": 2,
          "duration_hours": 6,
          "pattern": "constant",
          "description": "",
          "triggers": "",
          "relief": "",
          "notes": ""
        }
      ]
    }
  ]
}
```

## Dataset lifecycle

- A new dataset is created at interactive app launch and includes an empty
  report.
- Edits are flushed after the UI change.
- Export and upload finalize the report before creating the archive.
- `manifest.json` includes `symptom_cards`, `symptom_subsections` and
  `has_symptom_report`.
- The report is hashed/signed with the other dataset files and included in the
  archive.
- Queueing an upload rotates to a new dataset and starts with an empty report.
- Report-only datasets are exportable and uploadable.
