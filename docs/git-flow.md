# Git flow and release process

This repository uses git flow. Do not bump versions in commits and do not create
release tags or GitHub releases outside the process below.

## Branches

| Branch | Purpose |
|---|---|
| `main` | Production history. Only merged release branches and tagged release commits. |
| `develop` | Integration branch for the next release. |
| `feature/xyz` | One feature, branched from `develop`. |
| `release/x.y.z` | Release stabilization, branched from `develop`. |
| `hotfix/x.y.z` | Optional emergency fix from `main`; merge back into `main` and `develop`. |

## Planning phase

At the start of a release cycle we plan together:

1. Define the features and intended functionality for the next release.
2. Write each feature to `RELEASE.md` with a short description.
3. Add the commit at which the feature branch was last updated once the feature
   is complete.
4. When planning is finished, begin implementing features one by one.

`RELEASE.md` is the release checklist, for example:

```markdown
# Release v1.0.0

## Planned features

| Feature | Description | Completed commit |
|---|---|---|
| `feature/upload-resume` | Resume interrupted B2 uploads | `abc1234` |
| `feature/export-csv` | Export readings as CSV | `def5678` |
```

## Feature work

```bash
git checkout develop
git pull --ff-only
git checkout -b feature/xyz
```

Commit changes on `feature/xyz`. When we agree the feature is complete:

```bash
git checkout develop
git merge --no-ff feature/xyz
git push origin develop
```

Update `RELEASE.md` with the feature branch's final commit hash. Do not tag or
publish from a feature branch.

## Release branch and build check

When every planned feature is merged into `develop`:

```bash
git checkout develop
git pull --ff-only
git checkout -b release/1.0.0
git push origin release/1.0.0
```

A push to `release/**` runs the multi-platform workflow but **does not create a
GitHub release**. Fix build errors on the release branch, commit, and push until
macOS, Linux and Windows are green.

## Final tag, main, and release

After the release branch is green:

```bash
git tag -a v1.0.0 -m "CellScope v1.0.0" release/1.0.0
git checkout main
git merge --ff-only release/1.0.0
git push origin v1.0.0 main
```

- `release/1.0.0` must be a descendant of `main` so the merge is a
  fast-forward and the tag remains on the `main` tip.
- If a fast-forward is impossible, merge with `--no-ff` and move the tag to the
  resulting merge commit before pushing main.
- Pushing `main` with the exact semantic tag on `HEAD` is the only event that
  publishes binaries to GitHub Releases.
- A main push without a tag is build-only and does not publish.

Then merge `main` back into `develop` and delete the release branch:

```bash
git checkout develop
git merge --no-ff main
git push origin develop
git branch -d release/1.0.0
git push origin --delete release/1.0.0
```

## CI behavior

- `release/**` push: build and test all platforms, upload workflow artifacts,
  no GitHub release.
- `main` push with a semantic tag exactly on `HEAD`: build, test, package and
  publish a GitHub release whose title is `CellScope vX.Y.Z`.
- `main` push without a tag: build and test only.
- Pull requests to `main` or `develop`: build and test only.
- Manual `workflow_dispatch`: build and test only.

## Rules

- Never commit a version bump.
- Never tag or publish before the release branch is green.
- Never publish from `feature/**` or `develop`.
- Do not use `--force` on shared branches.
- Keep `RELEASE.md` current with feature descriptions and completed commit
  hashes.
