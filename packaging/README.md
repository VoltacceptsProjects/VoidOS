# Publishing voidwm via a real APT repository

This turns `voidwm` into `sudo apt install voidwm`, with `apt upgrade`
picking up new releases automatically. It uses `reprepro` to maintain
a signed Debian repo and GitHub Pages to host it as flat files (no
server needed — apt just fetches over HTTPS).

## What's included

- `packaging/build-deb.sh` — builds `voidwm_<version>_amd64.deb` from
  this source tree via the existing `make install` target.
- `packaging/control.in`, `packaging/copyright`, `packaging/changelog`
  — package metadata, filled in by `build-deb.sh`.
- `.github/workflows/release.yml` — on every `vX.Y.Z` tag, builds the
  `.deb` and adds it to the APT repo automatically.
- `apt-repo/` (delivered alongside this, not inside the source tree)
  — a working `reprepro` repo already containing `voidwm 1.0.0`,
  ready to be pushed as-is to a `gh-pages`-style host.

## One-time setup

### 1. Host the repo

Simplest option: an orphan branch of this same repo, e.g. `apt-repo`,
served by GitHub Pages.

```sh
cd apt-repo               # the directory delivered alongside this repo
git init
git checkout -b apt-repo
git add -A
git commit -m "voidwm 1.0.0"
git remote add origin git@github.com:<you>/<this-repo>.git
git push -u origin apt-repo
```

Then in the repo's Settings -> Pages, set the source to the `apt-repo`
branch, root folder. Your repo URL becomes:

```
https://<you>.github.io/<this-repo>/
```

(A dedicated repo works the same way — just push there instead and
skip the branch step.)

### 2. Keep the signing key safe

A key was generated for you during initial packaging (see the
separately-delivered `voidos-signing-key.asc` — **do not commit this
file anywhere**, it's the private half). To use it in CI, first
confirm the key ID:

```sh
gpg --list-secret-keys --keyid-format=long
```

In the GitHub repo running the workflow, add these **Actions
secrets** (Settings -> Secrets and variables -> Actions):

| Secret | Value |
|---|---|
| `APT_SIGNING_KEY` | contents of `voidos-signing-key.asc` (the private key, armored) |
| `APT_SIGNING_KEY_ID` | the long key ID, e.g. `AD2CB31D9BCDAD89AB82B5ED877CB54D6A5B546B` |
| `APT_REPO_TOKEN` | only needed if the APT repo lives in a *different* GitHub repo than the source — a PAT with push access to it. If it's a branch of this same repo, the default `GITHUB_TOKEN` works and you can skip this, as long as the workflow has `permissions: contents: write`. |

**Strongly recommended:** since this key was generated in a shared
build environment, treat it as already-exposed. Generate your own
replacement before going further — run this on your own machine:

```sh
gpg --batch --gen-key /dev/stdin <<KEYSPEC
%no-protection
Key-Type: RSA
Key-Length: 4096
Key-Usage: sign
Name-Real: VoidOS Package Signing
Name-Email: you@yourdomain.example
Expire-Date: 2y
%commit
KEYSPEC
```

Then re-export the public key into `apt-repo/voidos-archive-keyring.asc`
(`gpg --export --armor <key-id>`), and update the two secrets above to
match. `reprepro` will re-sign the repo with the new key on the next
`includedeb`.

### 3. Update the placeholder URL

`packaging/control.in`'s `Homepage:` field and `apt-repo/index.html`
both have a `<your-gh-username>/<your-apt-repo>` placeholder — replace
with your actual Pages URL from step 1.

### 4. Cut a release

```sh
git tag v1.1.0
git push origin v1.1.0
```

The workflow builds the `.deb`, adds it to the `apt-repo` branch, and
pushes. Anyone with the repo already added gets it on their next
`apt update && apt upgrade`.

## What end users do (one time)

```sh
curl -fsSL https://<you>.github.io/<repo>/voidos-archive-keyring.asc \
  | sudo gpg --dearmor -o /usr/share/keyrings/voidos-archive-keyring.gpg

echo "deb [signed-by=/usr/share/keyrings/voidos-archive-keyring.gpg] https://<you>.github.io/<repo> stable main" \
  | sudo tee /etc/apt/sources.list.d/voidos.list

sudo apt update
sudo apt install voidwm
```

From then on, `sudo apt upgrade` picks up new voidwm versions
automatically, same as any other package.

## Verified locally

Before delivery this was tested end-to-end in a real environment: the
`.deb` was built with `build-deb.sh`, added to a `reprepro` repo,
served over `file://`, added as an apt source with `signed-by`, and
installed with a plain `sudo apt install voidwm` — dependency
resolution (including pulling in a terminal emulator via the
`x-terminal-emulator` virtual package) worked correctly.
