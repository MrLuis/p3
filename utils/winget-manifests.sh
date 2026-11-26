#!/usr/bin/env bash
# Writes the winget manifests (microsoft/winget-pkgs) for a published p3
# release: <output dir>/manifests/m/MrLuis/p3/<version>/, the version,
# installer and en-US locale files, ready for `winget validate` and a pull
# request to winget-pkgs (copy the manifests directory into a clone).
#
#   utils/winget-manifests.sh <version> [<output dir>]
#
# The installer is the release's Windows zip; its SHA-256 is read from the
# .sha256 file published next to it. p3 is a portable program: winget
# unpacks the whole zip, and since p3.exe finds its libraries relative to
# itself, it puts the zip's bin directory on PATH
# (ArchiveBinariesDependOnPath) rather than linking p3.exe alone.
set -euo pipefail

fail() { echo "winget-manifests: $*" >&2; exit 1; }

[ $# -ge 1 ] || fail "usage: $0 <version> [<output dir>]"
version=${1#v}
out=${2:-winget}
repo=https://github.com/MrLuis/p3
name=p3-$version-windows-x86_64
url=$repo/releases/download/v$version/$name.zip

sha=$(curl -fsSL "$url.sha256" | cut -d' ' -f1) ||
  fail "cannot read $url.sha256 (is v$version published, with the Windows zip?)"
[[ $sha =~ ^[0-9a-f]{64}$ ]] || fail "not a SHA-256 in $url.sha256: $sha"
sha=$(echo "$sha" | tr a-f A-F)

dir=$out/manifests/m/MrLuis/p3/$version
mkdir -p "$dir"
schema=1.10.0
header() {
  echo "# yaml-language-server: \$schema=https://aka.ms/winget-manifest.$1.$schema.schema.json"
  echo
}

{
  header version
  cat <<EOF
PackageIdentifier: MrLuis.p3
PackageVersion: $version
DefaultLocale: en-US
ManifestType: version
ManifestVersion: $schema
EOF
} > "$dir/MrLuis.p3.yaml"

{
  header installer
  cat <<EOF
PackageIdentifier: MrLuis.p3
PackageVersion: $version
InstallerType: zip
NestedInstallerType: portable
NestedInstallerFiles:
- RelativeFilePath: $name\\bin\\p3.exe
  PortableCommandAlias: p3
ArchiveBinariesDependOnPath: true
MinimumOSVersion: 10.0.17763.0
Installers:
- Architecture: x64
  InstallerUrl: $url
  InstallerSha256: $sha
ManifestType: installer
ManifestVersion: $schema
EOF
} > "$dir/MrLuis.p3.installer.yaml"

{
  header defaultLocale
  cat <<EOF
PackageIdentifier: MrLuis.p3
PackageVersion: $version
PackageLocale: en-US
Publisher: Luis Ortega
PublisherUrl: https://github.com/MrLuis
PublisherSupportUrl: $repo/issues
PackageName: p3
PackageUrl: $repo
License: Apache-2.0 WITH LLVM-exception
LicenseUrl: $repo/blob/v$version/LICENSE
ShortDescription: A Pascal compiler built on LLVM
Description: |-
  p3 compiles Pascal (ISO 7185, Turbo/Free Pascal and Object Pascal dialects)
  to native programs with LLVM. The Windows package is self-contained: it links
  programs itself, with the MinGW-w64 libraries it carries, so nothing else needs
  to be installed.
Moniker: p3
Tags:
- compiler
- llvm
- pascal
ReleaseNotesUrl: $repo/releases/tag/v$version
ManifestType: defaultLocale
ManifestVersion: $schema
EOF
} > "$dir/MrLuis.p3.locale.en-US.yaml"

echo "winget-manifests: $dir"
ls "$dir"
