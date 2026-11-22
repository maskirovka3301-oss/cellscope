# Muscle code atlas (draft)

The anatomy diffuse atlas paints each muscle with a flat color and a white
short code. Picking samples the diffuse color at the hit UV and maps it to the
nearest code below. This is a **draft for review**, not an authoritative
anatomical table.

| Code | Draft body region |
|---|---|
| GMa | Gluteal |
| G | Gluteal |
| F | Fascia |
| LD | Back |
| Pso | Hip |
| TFL | Hip |
| BF | Thigh |
| VL | Thigh |
| RF | Thigh |
| AM | Thigh |
| RA | Abdomen |
| SA | Thorax |
| T | Lower leg |
| TA | Lower leg |
| SM | Lower leg |
| IB | Leg |
| SY | Leg |
| Om | Shoulder |
| S | Arm |
| C | Arm |
| SB | Arm |
| FCR | Forearm |
| FDS | Forearm |
| LE | Forearm |
| EDM | Forearm |
| MUS | Unmapped color |

## Picking behaviour

- The click ray is tested against every triangle of both primitives.
- The closest hit returns the 3D point and barycentric UV.
- The diffuse color at that UV selects the nearest code.
- Unmapped colors produce `MUS`; they are never silently assigned a different
  muscle code.
- The selected code, region and 3D point are stored on the symptom card.
