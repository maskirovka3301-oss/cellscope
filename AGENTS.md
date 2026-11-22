# Project workflow

Before planning a release, creating branches, committing, tagging, or publishing,
read [`docs/git-flow.md`](docs/git-flow.md) and [`RELEASE.md`](RELEASE.md).

Do not bump versions in commits and do not create releases or tags outside the
git-flow process. Feature work belongs on `feature/xyz`, integration on
`develop`, stabilization on `release/x.y.z`, and binary publishing only from a
tagged `main` push.
