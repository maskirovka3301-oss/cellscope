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

## Pre-release review gate

Before creating a release branch, the integrated `develop` build must be reviewed
with the user.

1. Build the current `develop` state.
2. Show the user the build, screenshots or affected screens, and a summary of the
   user-visible changes.
3. Collect the final cosmetic fixes and small changes.
4. Apply those fixes, rebuild, and repeat until the user explicitly says they are
   happy with the result.
5. Only then create and push `release/x.y.z`.

Do not create, merge or push a release branch before this sign-off. A passing CI
build is not approval. The back-and-forth review is mandatory for every release
and may take several rounds. Feature integration into `develop` continues
normally during this phase; the gate is specifically before `develop` becomes a
release branch.

When every planned feature is merged into `develop` and the review gate has
passed:

```bash
git checkout develop
git pull --ff-only
git checkout -b release/1.0.0
git push origin release/1.0.0
```

A push to `release/**` runs the multi-platform workflow. If the macOS, Linux
and Windows builds pass, the workflow creates the `vX.Y.Z` tag and a GitHub
release with the packaged binaries. Fix build errors on the release branch,
commit, and push until all platforms are green and the release appears.

## Merge the successful release into main

Once the release workflow is green and the tagged GitHub release exists, merge
the release branch into `main` and push it:

```bash
git checkout main
git pull --ff-only
git merge --ff-only release/1.0.0
git push origin main
```

- `release/1.0.0` must be a descendant of `main`, so the merge is a
  fast-forward and the tag remains on the `main` tip.
- The `main` push rebuilds all platforms and refreshes the same `v1.0.0`
  release with binaries built from the final main commit.
- If a fast-forward is impossible, merge with `--no-ff`, move the tag to the
  merge commit, and push both; do not create a second release tag.

Then merge `main` back into `develop` and delete the release branch:

```bash
git checkout develop
git merge --no-ff main
git push origin develop
git branch -d release/1.0.0
git push origin --delete release/1.0.0
```

## CI behavior

- `release/**` push: build and test all platforms. If all pass, create the
  `vX.Y.Z` tag and publish the GitHub release with packaged artifacts.
- `main` push with a semantic tag exactly on `HEAD`: rebuild and refresh the
  release assets from the merged main commit.
- `main` push without a tag: build and test only.
- Pull requests to `main` or `develop`: build and test only.
- Manual `workflow_dispatch`: build and test only.

## Rules

- Never commit a version bump.
- Never tag or publish before the release branch is green.
- Never publish from `feature/**` or `develop`; the workflow does it for
  `release/**` and refreshes it from tagged `main`.
- Only use `release/x.y.z`; the workflow rejects other release branch names.
- Do not use `--force` on shared branches.
- Keep `RELEASE.md` current with feature descriptions and completed commit
  hashes.
- Do not start a release branch until the user has explicitly approved the
  integrated `develop` build after the cosmetic/small-change review.
