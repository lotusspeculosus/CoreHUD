// Tests for the auto-updater's pure logic: version comparison, SHA-256, SHA256SUMS.txt parsing.
#include <cstdio>
#include <string>
#include <vector>
#include "../src/updater.h"
#include "../src/version.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static std::vector<unsigned char> Bytes(const std::string& s) { return std::vector<unsigned char>(s.begin(), s.end()); }

int main() {
    // version.h: the string and the numbers (Windows file version) must agree
    {
        char v[32];
        snprintf(v, sizeof v, "%d.%d.%d", COREHUD_VER_MAJOR, COREHUD_VER_MINOR, COREHUD_VER_PATCH);
        CHECK(std::string(v) == COREHUD_VERSION_STR);
    }
    // versions
    CHECK(upd::CompareVersions("1.2.0", "1.2.0") == 0);
    CHECK(upd::CompareVersions("v1.2.0", "1.2.0") == 0);
    CHECK(upd::CompareVersions("1.2.1", "1.2.0") > 0);
    CHECK(upd::CompareVersions("1.10.0", "1.9.9") > 0);     // numeric, not text
    CHECK(upd::CompareVersions("2.0", "1.99.99") > 0);
    CHECK(upd::CompareVersions("1.2", "1.2.0") == 0);
    CHECK(upd::CompareVersions("1.1.0", "1.2.0") < 0);
    CHECK(upd::CompareVersions("v1.3.0-beta", "1.2.0") > 0);

    // SHA-256 test vectors (FIPS 180-2)
    CHECK(upd::Sha256Hex(Bytes("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(upd::Sha256Hex(Bytes("")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(upd::Sha256Hex(Bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    // SHA256SUMS.txt parsing
    std::string sums =
        "3F65D25985BC9F0C30C0B36C3804C4371EA6DD0869D3E8AD63BF38016A3FE4AE  CoreHUD-1.2.0-win64.zip\r\n"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa  CoreHUD.exe.old\n"
        "2EA77E9DEF81698932D4A6191EE91B3F1DD9BE9405936C7BE9693A4997077DED *CoreHUD.exe\n"
        "notahash  corehud-cli.exe\n"
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb  corehud-cli.exe   \n";
    CHECK(upd::ExpectedSha(sums, "CoreHUD.exe") == "2ea77e9def81698932d4a6191ee91b3f1dd9be9405936c7be9693a4997077ded");
    CHECK(upd::ExpectedSha(sums, "corehud-cli.exe") == "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    CHECK(upd::ExpectedSha(sums, "missing.exe").empty());
    CHECK(upd::ExpectedSha("", "CoreHUD.exe").empty());
    // malformed lines (hash with no file name) must be skipped, not crash
    CHECK(upd::ExpectedSha("2ea77e9def81698932d4a6191ee91b3f1dd9be9405936c7be9693a4997077ded *\n", "CoreHUD.exe").empty());
    CHECK(upd::ExpectedSha("2ea77e9def81698932d4a6191ee91b3f1dd9be9405936c7be9693a4997077ded \t*\r\n"
                           "2ea77e9def81698932d4a6191ee91b3f1dd9be9405936c7be9693a4997077ded  CoreHUD.exe\n", "CoreHUD.exe") ==
          "2ea77e9def81698932d4a6191ee91b3f1dd9be9405936c7be9693a4997077ded");

    printf("\nupdate: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
