# Symptom catalog

The provisional catalog lives at `assets/symptom-catalog.txt` and is copied into
the application assets. It is line-oriented so the taxonomy can be replaced
without touching card code.

```text
# label|control,control,...
Pain|severity,onset,duration,pattern,description,triggers,relief,notes
Vision change|onset,duration,pattern,description,triggers,relief,notes
```

Supported controls:

| Key | UI |
|---|---|
| `severity` | 0–10 severity stepper |
| `onset` | hours ago, 0–24 |
| `duration` | hours, 0–24 |
| `pattern` | constant / intermittent / occasional |
| `description` | free text |
| `triggers` | free text |
| `relief` | free text |
| `notes` | free text |

A missing or unreadable file falls back to the built-in provisional list. The
catalog is loaded once at app startup; fuzzy matching and the per-symptom
controls come from this file.
