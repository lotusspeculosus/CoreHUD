// Auto-update from GitHub Releases (github.com/lotusspeculosus/CoreHUD).
//  - Check: GET https://api.github.com/repos/<repo>/releases/latest (tag "vX.Y.Z", drafts/pre-releases ignored)
//  - Download: the single release asset CoreHUD.exe (everything is in one program; HTTPS, GitHub hosts only)
//  - Verify: its SHA-256 must match the digest GitHub publishes for the asset (or SHA256SUMS.txt as a fallback,
//    which releases keep for older CoreHUD versions), and it must be a valid PE file
//  - Apply: rename the running exe to *.old (Windows allows renaming a running exe), move the new one in,
//    start it with --wait-pid, exit. Leftover *.old files are removed on the next start.
// Only requests to GitHub are made; nothing about the PC is sent (just a "CoreHUD/<version>" user agent).
#pragma once
#include <string>
#include <vector>

namespace upd {

enum Mode { MODE_OFF = 0, MODE_NOTIFY = 1, MODE_AUTO = 2 };

struct Release {
    std::string tag;          // "v1.2.1"
    std::string version;      // "1.2.1"
    std::string pageUrl;      // html_url
    std::wstring notes;       // release body (first lines)
    std::string exeUrl, sumsUrl;
    std::string exeSha;       // GitHub's asset digest ("sha256:<hex>" -> hex), empty if not provided
};

int CompareVersions(const std::string& a, const std::string& b);   // <0 a older, 0 same, >0 a newer
bool FetchLatest(const std::string& repo, Release& out, std::string& err);
// Downloads + verifies into "<exe dir>\update\" (staged files). Returns false with a message on any problem.
bool Stage(const Release& r, std::string& err);
bool IsStaged(const std::string& version);
// Swaps the staged exe(s) in and starts the new CoreHUD. Caller must exit right after it returns true.
bool ApplyStaged(std::string& err);
void CleanupOld();            // remove *.old leftovers from a previous update
std::string Sha256Hex(const std::vector<unsigned char>& data);
// Looks up "<64 hex chars>  <file name>" in a SHA256SUMS.txt text; returns lower-case hex or "".
std::string ExpectedSha(const std::string& sumsText, const std::string& fileName);

} // namespace upd
