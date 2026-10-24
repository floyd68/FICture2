# FICture2 working conventions

## Task tracking: GitHub Issues

- Use https://github.com/floyd68/FICture2/issues as the sole persistent task
  tracker. Issue scope, acceptance criteria, dependencies, and current comments
  take precedence over historical local task notes.
- Read the relevant issue before implementation. For the code-review work,
  start with tracking issue #10 and follow its linked issues and dependencies.
- Record implementation progress, blockers, verification results, and completion
  in the relevant issue. Reuse existing issues rather than creating duplicates.
- If GitHub is unavailable, report the limitation. Temporary planning inside
  the current conversation is allowed.
- The repository is public: keep issue/PR text free of local absolute paths,
  credentials and private data, and do not write bare `@name` mentions.
- Write issue titles, issue bodies, PR titles, and PR bodies in English only.

## Labels

| Label | Meaning |
|---|---|
| `priority:high` / `priority:medium` / `priority:low` | Work order |
| `tracking` | Umbrella issue that orders a group of issues |
| `blocked` | Waiting on another issue or PR; skip when picking work |
| `changes-requested` | Review found problems; fix on the same PR branch |
| `approved` | Review passed; ready to merge |
| `needs-manual-test` | Requires running the app; local build/tests cannot verify it |
| `submodule` | Needs a change in FD2D, ImageCore, Floar or ThumbnailProvider |
| `ci` | Build / CI infrastructure |

## Repository layout and submodules

- First-party submodules, each its own repository with its own history:
  `FD2D` (UI/render framework), `ImageCore` (decoding/scheduling), `Floar`
  (virtual file system/archives), `ThumbnailProvider` (shell thumbnail DLL).
  `external/DirectXTex` and `external/spdlog` are third-party; do not modify them.
- A change inside a first-party submodule is made in that repository: branch
  and commit there (open a PR in that repository when it is published on
  GitHub), then bump the pin in FICture2 in the same FICture2 PR that consumes
  the change. Commit messages for pin bumps follow the existing style
  (`Bump <Submodule> pin(s); <reason>.`).
- Never leave FICture2 pinned to a submodule commit that is not pushed to the
  submodule's remote.
- Put tests for submodule code in the submodule repository.
- Issues for submodule code live in the submodule repository. A submodule
  issue is complete only when FICture2 pins the pushed commit: the submodule PR
  references it with `Refs #N`, and the FICture2 PR that pins the commit closes
  it with `Fixes floyd68/<repo>#N` and lists the pinned commit. Each submodule
  issue names its consumer (a FICture2 issue or a standalone pin-update PR) in
  its "FICture2 integration" section. A standalone pin-update PR is allowed
  without a FICture2 issue; it closes the submodule issue it consumes.

## Building and verification

- Toolchain: Visual Studio 2026 (generator `Visual Studio 18 2026`), x64,
  C++20, CMake >= 3.21. Shipped builds use `MinSizeRel`.
- Local verification for every PR:
  ```
  cmake -S . -B build -G "Visual Studio 18 2026" -A x64 -DFICTURE2_BUILD_TESTS=ON -DIMAGECORE_BUILD_TESTS=ON
  cmake --build build --config Debug
  cmake --build build --config MinSizeRel
  ctest --test-dir build -C Debug --output-on-failure
  ```
  Report new warnings introduced by the change.
- FICture2 tests live in `tests/` and build with
  `-DFICTURE2_BUILD_TESTS=ON` (non-window tests only). Each repository gets its
  test option (default OFF) with its first test-bearing issue:
  `FICTURE2_BUILD_TESTS` (FICture2 #7), `IMAGECORE_BUILD_TESTS`, `FD2D_BUILD_TESTS`,
  `FLOAR_BUILD_TESTS`, and `THUMBNAILPROVIDER_BUILD_TESTS`; vendored
  dependency tests stay off. ImageCore, FD2D and ThumbnailProvider have no
  standalone CMake project and depend on the FICture2 tree, so their tests are
  enabled when configuring FICture2 (for example
  `-DIMAGECORE_BUILD_TESTS=ON`); Floar can also be built standalone. When test
  targets exist, also run
  `ctest --test-dir build -C Debug --output-on-failure` and treat failures as
  blocking. New logic that can be tested without a window (scheduler, binding,
  IPC queue, caches, path handling) should come with tests once the test option
  exists.
- There is no CI yet. Until a CI workflow lands, the local Debug and MinSizeRel
  builds above are the required checks, and the reviewer re-runs them. After
  CI lands, its required checks replace this rule.
- GUI behaviour (rendering, input, device loss, drag & drop, IPC between
  instances, file associations) needs `needs-manual-test` with concrete steps.

## Command: "진행" (implement)

1. **Fix rejected work first.** If an open PR carries `changes-requested`, or an
   issue has unresolved review feedback, address that before anything new:
   push fixes to the same branch, reply on the PR with what changed, remove
   `changes-requested`.
2. Otherwise pick the next issue: not `blocked`, highest `priority:*`, then the
   order in the tracking issue, then the lowest number. Keep **at most one open
   implementation unit** at a time to avoid conflicts; a submodule PR and the
   FICture2 PR that consumes it count as one unit.
3. Comment on the issue that work has started; create branch
   `issue-<N>-<short-slug>` from up-to-date `master` (and the same branch name
   in any submodule that needs changes).
4. Implement, then run the verification in "Building and verification".
   Follow the issue's acceptance criteria; user-visible changes update README
   (the current `What's New` section and the relevant feature/hotkey/INI
   sections) in the same PR.
5. Open a PR whose body contains `Fixes #N` (`Refs #N` for a submodule PR;
   `Fixes floyd68/<repo>#N` in the FICture2 PR that pins it), a summary,
   verification results (commands and outcomes), submodule commits and pin
   changes if any, and any
   remaining risks. Add `needs-manual-test` with concrete manual steps when the
   change cannot be verified without running the app.
6. Record progress / verification on the issue. Do not merge your own PR in
   this step.

## Command: "리뷰" (review)

1. Review the open PR together with its linked issue. Review from a fresh
   context (a reviewer without the implementation conversation, e.g. a separate
   agent given only the diff and the issue), checking the diff against the
   issue's acceptance criteria. Include submodule diffs referenced by pin bumps.
2. Re-run the required checks (local builds now, CI once it exists); a failing
   build or test is a review failure.
3. Check the branch is up to date with `master` and merges cleanly, and that
   every bumped submodule commit exists on that submodule's remote.
4. **Problems found:** leave review comments on the PR (inline where possible),
   add `changes-requested`, keep the PR open. Problems outside the PR's scope go
   into a new issue (or the relevant existing issue) instead.
5. **No problems:** add `approved`, squash-merge (never with `--admin`), delete
   the branch, confirm the issue closed via `Fixes #N` (for submodule issues,
   when the pinning FICture2 PR merges), and tick the item in the tracking
   issue. The reviewer is authorized to merge every PR that passes
   review, except a `needs-manual-test` PR, which waits until the manual test
   is reported done.
6. GitHub does not allow approving or requesting changes on a PR authored by
   the same account; review state is expressed with labels and comments.

## Command: "배포" (release)

Issued by the maintainer only after they have checked `master`; it releases
the latest `master` commit. Never start a release without this command.

1. **Preflight:** local `master` equals `origin/master`, the working tree is
   clean, every submodule is at its pinned commit and that commit is pushed, no
   implementation PR is half-merged, and the Debug/MinSizeRel builds (or CI,
   once it exists) are green on that commit. Otherwise stop and report.
2. **Version:** CMake derives `MAJOR.MINOR.<commit count>.<build>` from
   `FICTURE2_VERSION_MAJOR/MINOR` in `CMakeLists.txt` and
   `git rev-list --count HEAD`; tags look like `v1.7.142.0`. A MAJOR/MINOR bump
   goes through a normal PR beforehand (`Bump project version baseline to X.Y.`),
   never in the release commit.
3. **Release docs:** check README's `What's New` section and
   `NexusMods_Mod_Description.bbcode` against the PRs merged since the last
   `v*` tag; if they need changes, commit only those docs on `master` as
   `Release <version>: <summary>`, computing `<version>` from the commit count
   that includes this commit. Never force-push `master`.
4. **Build and publish:** build the installer with the existing scripts
   (`build_winget.bat` + `pack_winget.bat` for the Inno Setup installer
   `FICture2-Setup-<MAJOR.MINOR>.exe`), tag `v<version>` on the release commit,
   push the tag, and create the GitHub Release with `gh release create`
   (title `FICture2 v<version>`, installer attached, notes from that version's
   README `What's New` items).
5. **On failure:** act on the actual remote state, not the error message.
   Check `gh release view v<version>`:
   - no release: fix the cause (code goes through the normal issue → PR flow),
     then re-tag and retry; delete only a local tag created by this attempt
     that does not exist remotely;
   - leftover draft or partial assets: delete the draft, keep the tag if it
     still points at the release commit, retry;
   - already published: never delete or overwrite it automatically; report
     and let the maintainer decide.
6. Winget submission (`release_winget.bat`, `submit_winget.bat`), the Microsoft
   Store package (`build_store.bat`) and the Nexus upload stay manual
   (maintainer) unless explicitly requested. Report the installer to use.

## Commits

- One logical change per commit; never mix bug fixes with restructuring.
- No `Co-Authored-By` trailers for Claude.
