# Code signing policy

Free code signing provided by [SignPath.io](https://signpath.io), certificate by [SignPath Foundation](https://signpath.org).

> **Status:** the application to the SignPath Foundation is in progress. Until it's approved, releases are not signed yet;
> this page describes how signing works once it is.

## What gets signed

Only `CoreHUD.exe`, built from the source code in this repository. Every signed exe is:

- built by the [Release workflow](.github/workflows/release.yml) on a GitHub-hosted Windows runner, from the tagged
  commit of this public repository, after the unit tests pass;
- submitted to SignPath straight from that workflow (SignPath checks that the file comes from that build), with the
  file metadata locked to product name `CoreHUD` and the version being released
  ([artifact configuration](.signpath/artifact-configuration.xml));
- signed only after a maintainer approves the signing request in SignPath, for every single release.

CoreHUD has no proprietary components. The only third-party code is the
[QR Code generator library](https://www.nayuki.io/page/qr-code-generator-library) by Project Nayuki (MIT license,
unmodified, in `src/third_party/qrcodegen`), compiled into the exe.

## Team roles

| Role | Members |
|---|---|
| Committers and reviewers | [lotusspeculosus](https://github.com/lotusspeculosus) |
| Approvers | [lotusspeculosus](https://github.com/lotusspeculosus) |

Every change from someone outside the team, including pull requests proposed by the automated issue assistant, is
reviewed by a committer before it is merged. All team members use multi-factor authentication for GitHub and SignPath.

## Privacy policy

CoreHUD runs locally and has no telemetry, accounts or ads. It does not collect personal data. The network
connections it can make are:

| What | When | What is sent | How to turn it off |
|---|---|---|---|
| **Update check** to `api.github.com` (this repository's latest release), and the download of a new `CoreHUD.exe` from GitHub | 20 seconds after start and every 6 hours | Only a standard HTTPS request with the user agent `CoreHUD/<version>`. Nothing about your PC. | Settings &rsaquo; Updates &rsaquo; **Off** (or *Notify me*) |
| **LibreHardwareMonitor** web server on `127.0.0.1` (this PC only) | While CoreHUD runs | Nothing leaves the PC | Don't run LibreHardwareMonitor, or turn its web server off |
| **Tablet / phone panel**: a small read-only web server on your PC | Only after you switch it on (off by default) | The panel image, to devices on your own network or USB that know the secret key | Sensor panel &rsaquo; Tablet / phone &rsaquo; **Off** |
| **Install latest** in the Components check (runs Windows' `winget`) | Only when you click it | What `winget` itself sends to its package sources | Don't click it |

**Copy diagnostics** in Settings only copies text (hardware and sensor readings, nothing personal) to your clipboard;
you decide whether to paste it anywhere. Opening the GitHub page or a download page from CoreHUD opens your browser.

All other data stays on your PC: settings (`CoreHUD.ini`), layouts (`layouts\`), logs, black box recordings and the
wear history (`logs\`), next to the exe.

## Reporting problems

Report a problem with a signed CoreHUD release, or anything that looks like misuse of the certificate, privately via
[Report a vulnerability](https://github.com/lotusspeculosus/CoreHUD/security/advisories/new).
