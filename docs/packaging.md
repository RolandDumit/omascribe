# Omascribe packages and releases

The first Omascribe release is 0.7.0, continuing Omawrite's version sequence.
The current release is 1.0.0; its Arch package is `omascribe` 1.0.0-1. The executable, icon, desktop entry,
Qt application identity, settings, recovery and local control use Omascribe's
own names. The package can coexist with the official `omawrite`; it does not
provide, replace or conflict with that package. Original license notices are
retained. Existing Omawrite settings and drafts are not automatically imported.

## Install this working checkout

From the repository in a terminal:

```sh
bin/harness check
bin/install
omascribe --version
omascribe
```

Run `bin/install` as your ordinary user. It uses `makepkg -fsi`, which builds
the current working tree and invokes pacman for installation and missing
dependencies. It rebuilds even when a package with the same version exists.
It installs `/usr/bin/omascribe` and the Omascribe launcher and icon; there is
no manual copy to `/usr/bin`.

For a build without installing or changing system packages:

```sh
cd pkgbuild
makepkg -f
pacman -Qip omascribe-1.0.0-1-x86_64.pkg.tar.zst
pacman -Qlp omascribe-1.0.0-1-x86_64.pkg.tar.zst
```

Use the clone on the development machine to test unpublished changes. Rerun
`bin/install` when you want those changes in the installed app. A local build
has a version but no automatic connection to GitHub pushes. `bin/harness run`
remains the way to test without using personal settings or drafts.

## Publish stable releases on AUR

For everyday installations and other machines, use a release package on AUR
once available. AUR supplies the recipe; the user's machine compiles and
installs it. A local installation with the same package name can subsequently
receive higher published AUR versions through Omarchy's yay update step.

The checked-in `pkgbuild/PKGBUILD` packages its parent working checkout and has
an empty `source` array. Keep it for local development. For every GitHub release,
publish a matching standalone AUR recipe; the local recipe must not be submitted
unchanged to AUR.

### Current AUR status

On 2026-10-09 the maintainer reported that AUR has closed new account
registrations. They do not yet have an AUR account, so publication is deferred.
The verified 1.0.0 recipe is included in the GitHub release as
`omascribe-1.0.0-1-aur.zip`, alongside the installable Arch package and checksums.
Use the development checkout or the GitHub package while AUR access is unavailable.
These installations need manual updates until the package is published on AUR.

Continue preparing and verifying the AUR recipe for each GitHub release and
record the unpublished step. When the maintainer confirms account and SSH access,
publish the latest verified recipe. The existing GitHub tags remain unchanged.

## Mandatory release workflow

A release is complete only when both GitHub and AUR contain the new version.
Changing a version requires updating `src/appversion.h`, `pkgbuild/PKGBUILD` and
the bold release version in the README together. `bin/harness check` rejects
inconsistent versions. New software versions reset `pkgrel` to 1; packaging-only
fixes increment it.

1. Update versions and release notes, run `bin/harness check`, review evidence,
   and commit/push all release changes.
2. Create the immutable `v<version>` tag on that verified commit and publish a
   GitHub release describing changes, verification and relevant limitations.
3. Download the source archive for the published tag from GitHub. Use that exact
   archive, not a local working-tree tarball. For 1.0.0:

   ```sh
   mkdir -p .harness/downloads
   curl --fail --location --output .harness/downloads/omascribe-1.0.0.tar.gz \
     https://github.com/RolandDumit/omascribe/archive/refs/tags/v1.0.0.tar.gz
   bin/harness prepare-aur --archive .harness/downloads/omascribe-1.0.0.tar.gz
   ```

4. `prepare-aur` checks the archive's application version and local package
   metadata against the checkout, calculates SHA-256, creates a standalone
   PKGBUILD and generates `.SRCINFO` under a unique `.harness/releases/` directory.
   It copies the archived install script and archive into that directory for
   verification. A higher `pkgrel` permits packaging fixes using the checkout's
   updated metadata and install script while keeping the original source archive.
   Changes with an unchanged `pkgrel` are rejected. Its report explicitly records
   `aurPublished: false`.
5. In the printed `aur/` directory, run `makepkg -f`. Inspect the resulting package
   metadata, launcher and executable. It must build without the development
   checkout as its parent. The generated recipe downloads the immutable tag
   archive, verifies its checksum and builds from `$srcdir/omascribe-<version>`.
6. With an AUR account and registered SSH public key, clone
   `ssh://aur@aur.archlinux.org/omascribe.git`. Copy only `PKGBUILD`, `.SRCINFO`
   and `omascribe.install` from the verified output, commit and push. Never push
   archives, binary packages, build output, credentials or `.harness` evidence.
7. Verify GitHub's tag/release and AUR's package version. Report both URLs. If
   access is missing, keep the verified files, report the exact unpublished step,
   and complete all independent authorized work without claiming AUR success.

The workflow runs locally and does not require a hosted automation or embedded
AI service. A future agent performing a release must carry out the AUR step; a
GitHub release alone does not complete publication. Once the higher version is
published on AUR, Omarchy's yay update step can offer it to existing installations
named `omascribe`. Ordinary pushes without a package-version update do not
trigger a stable-package upgrade.

An AUR maintainer account and registered SSH key are external prerequisites. Use
normal SSH host-key verification and the user's configured publishing identity.
Do not create credentials, register an account or change system SSH settings
silently. If a version has already been tagged, do not move its tag to fix
packaging: use a packaging-only `pkgrel` bump and keep the source checksum of
that original published archive.
