// Checks that need no game: run t8cine_tests.exe; it prints failures and exits non-zero if any.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "common/cinemath.h"
#include "common/config.h"
#include "common/framemath.h"
#include "launcher/game_files.h"
#include "mod/scan.h"

namespace fs = std::filesystem;
using namespace cine;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(condition)                                                                                                                   \
    do {                                                                                                                                   \
        ++g_checks;                                                                                                                        \
        if (!(condition)) {                                                                                                                \
            printf("FAILED %s:%d  %s\n", __FILE__, __LINE__, #condition);                                                                  \
            ++g_failures;                                                                                                                  \
        }                                                                                                                                  \
    } while (0)

static bool Near(double a, double b, double tolerance = 1e-6)
{
    return std::fabs(a - b) <= tolerance;
}

static void TestRotations()
{
    for (const Rot r : { Rot { 0, 0, 0 }, Rot { 30, 45, 0 }, Rot { -60, 170, 20 }, Rot { 10, -120, -35 } }) {
        const Rot back = ToRot(ToQuat(r));
        CHECK(Near(back.Pitch, r.Pitch, 1e-6));
        CHECK(Near(WrapAngle(back.Yaw - r.Yaw), 0, 1e-6));
        CHECK(Near(WrapAngle(back.Roll - r.Roll), 0, 1e-6));
    }
    const Vec3 f = ForwardOf({ 0, 90, 0 });
    CHECK(Near(f.X, 0) && Near(f.Y, 1));
    const Vec3 r = RightOf({ 0, 0, 0 });
    CHECK(Near(r.Y, 1));
    CHECK(Near(UpOf({ 0, 0, 0 }).Z, 1));
    CHECK(Near(WrapAngle(190), -170));
}

static std::vector<CameraKey> SampleKeys()
{
    return { { 0.0, { 0, 0, 100 }, { 0, 0, 0 }, 40 },
             { 1.0, { 200, 50, 120 }, { -10, 30, 0 }, 45 },
             { 1.5, { 300, 200, 100 }, { 5, 80, 10 }, 50 },
             { 4.0, { 0, 400, 180 }, { 20, 170, -10 }, 30 } };
}

static void TestPath()
{
    const auto keys = SampleKeys();

    // Passes exactly through every key.
    for (const CameraKey& key : keys) {
        const CameraSample s = SamplePath(keys, key.Time);
        CHECK(Near(s.Location.X, key.Location.X, 1e-6) && Near(s.Location.Y, key.Location.Y, 1e-6) && Near(s.Location.Z, key.Location.Z, 1e-6));
        CHECK(Near(s.Fov, key.Fov, 1e-6));
        CHECK(Near(WrapAngle(s.Rotation.Yaw - key.Rotation.Yaw), 0, 1e-3));
        CHECK(Near(s.Rotation.Pitch, key.Rotation.Pitch, 1e-3));
    }
    // Holds the ends.
    CHECK(Near(SamplePath(keys, -5).Location.X, 0));
    CHECK(Near(SamplePath(keys, 99).Location.Y, 400));

    // No jumps or kinks: speed either side of each key should match closely.
    const double e = 1e-4;
    for (size_t i = 1; i + 1 < keys.size(); ++i) {
        const double t = keys[i].Time;
        const Vec3 a = SamplePath(keys, t - e).Location, b = SamplePath(keys, t).Location, c = SamplePath(keys, t + e).Location;
        const Vec3 before = (b - a) * (1 / e), after = (c - b) * (1 / e);
        CHECK(Length(before - after) < Length(before) * 0.01 + 1.0);
    }

    // Rotation has no sudden flips: small time steps give small angle steps.
    Quat previous = ToQuat(SamplePath(keys, 0).Rotation);
    double largest = 0;
    for (double t = 0.002; t < 4.0; t += 0.002) {
        const Quat q = ToQuat(SamplePath(keys, t).Rotation);
        const double dot = std::min(std::fabs(Dot(previous, q)), 1.0);
        largest = std::max(largest, 2 * std::acos(dot) * 180.0 / Pi);
        previous = q;
    }
    CHECK(largest < 2.0);

    CHECK(SamplePath({}, 1).Fov == 45);
    const std::vector<CameraKey> single = { keys[1] };
    CHECK(Near(SamplePath(single, 50).Location.X, 200));
}

static void TestRelativeCamera()
{
    const FighterFrame frame = MakeFighterFrame({ -100, 0, 90 }, { 100, 0, 90 });
    CHECK(Near(frame.Separation, 200));
    CHECK(Near(frame.Mid.Z, 90));

    const Vec3 location { 30, 400, 150 };
    const Rot rotation { -5, -95, 0 };
    const RelativeCamera rel = ToRelative(frame, location, rotation, 42);
    CHECK(Near(rel.Side, 400) && Near(rel.Along, 30) && Near(rel.Height, 60));

    Vec3 outLocation;
    Rot outRotation;
    double outFov = 0;
    FromRelative(frame, rel, outLocation, outRotation, outFov);
    CHECK(Near(outLocation.X, 30) && Near(outLocation.Y, 400) && Near(outLocation.Z, 150));
    CHECK(Near(WrapAngle(outRotation.Yaw - rotation.Yaw), 0) && Near(outRotation.Pitch, -5) && Near(outFov, 42));

    // The same framing carries over when the pair turns.
    const FighterFrame turned = MakeFighterFrame({ 0, -100, 90 }, { 0, 100, 90 });
    FromRelative(turned, rel, outLocation, outRotation, outFov);
    const RelativeCamera again = ToRelative(turned, outLocation, outRotation, outFov);
    CHECK(Near(again.Side, rel.Side) && Near(again.Along, rel.Along) && Near(WrapAngle(again.YawOffset - rel.YawOffset), 0));

    RelativeCamera small = rel;
    small.Side += 10;
    CHECK(!IsCinematicJump(rel, small, 60, 8));
    RelativeCamera moved = rel;
    moved.Side -= 300;
    CHECK(IsCinematicJump(rel, moved, 60, 8));
    RelativeCamera turnedView = rel;
    turnedView.YawOffset += 40;
    CHECK(IsCinematicJump(rel, turnedView, 60, 8));
}

static void TestSpeedRamp()
{
    const std::vector<SpeedKey> keys = { { 1, 1.0 }, { 2, 0.05 }, { 4, 1.0 } };
    CHECK(Near(SampleSpeed({}, 3), 1.0));
    CHECK(Near(SampleSpeed(keys, 0), 1.0));
    CHECK(Near(SampleSpeed(keys, 2), 0.05));
    CHECK(Near(SampleSpeed(keys, 9), 1.0));
    // Even in log space: halfway between 1x and 0.05x is the geometric middle, and it never overshoots.
    CHECK(Near(SampleSpeed(keys, 1.5), std::sqrt(0.05), 1e-9));
    double lowest = 10, highest = 0, previous = 1.0, biggestStep = 0;
    for (double t = 1; t <= 4; t += 0.001) {
        const double speed = SampleSpeed(keys, t);
        lowest = std::min(lowest, speed);
        highest = std::max(highest, speed);
        biggestStep = std::max(biggestStep, std::fabs(speed - previous));
        previous = speed;
    }
    CHECK(lowest >= 0.05 - 1e-9 && highest <= 1.0 + 1e-9);
    CHECK(biggestStep < 0.01);
}

static void TestLinearPath()
{
    const auto keys = SampleKeys();
    const CameraSample middle = SamplePathLinear(keys, 0.5);
    CHECK(Near(middle.Location.X, 100) && Near(middle.Location.Y, 25) && Near(middle.Fov, 42.5));
    CHECK(Near(SamplePathLinear(keys, 99).Location.Y, 400));
}

static void TestLookAt()
{
    const Rot right = LookAt({ 0, 0, 0 }, { 0, 100, 0 });
    CHECK(Near(right.Yaw, 90) && Near(right.Pitch, 0));
    const Rot up = LookAt({ 0, 0, 0 }, { 100, 0, 100 });
    CHECK(Near(up.Yaw, 0) && Near(up.Pitch, 45));
    const Vec3 f = ForwardOf(LookAt({ 10, 20, 30 }, { 110, 20, 30 }));
    CHECK(Near(f.X, 1) && Near(f.Y, 0));
}

static void TestFrameMath()
{
    using namespace framemath;
    CHECK(BlendedSampleCount(16, 180) == 8);
    CHECK(BlendedSampleCount(16, 360) == 16);
    CHECK(BlendedSampleCount(1, 180) == 1);
    CHECK(BlendedSampleCount(4, 1) == 1);

    // Averaging 8-bit pixels.
    uint8_t a[8] = { 0, 100, 200, 255, 10, 20, 30, 255 };
    uint8_t b[8] = { 100, 100, 100, 255, 30, 40, 50, 255 };
    uint32_t sum[8] = {};
    AddSample(a, 2, PixelFormat::Bgra8, sum);
    AddSample(b, 2, PixelFormat::Bgra8, sum);
    uint8_t out[8];
    Average(sum, 2, PixelFormat::Bgra8, 2, out);
    CHECK(out[0] == 50 && out[1] == 100 && out[2] == 150 && out[3] == 255 && out[4] == 20 && out[6] == 40);

    // Averaging 10-bit pixels keeps the channels apart.
    const auto pack = [](uint32_t r, uint32_t g, uint32_t bl, uint32_t al) { return r | (g << 10) | (bl << 20) | (al << 30); };
    uint32_t c[1] = { pack(1023, 0, 100, 3) }, d[1] = { pack(1, 1000, 300, 3) };
    uint32_t sum10[4] = {};
    AddSample(reinterpret_cast<uint8_t*>(c), 1, PixelFormat::X2Bgr10, sum10);
    AddSample(reinterpret_cast<uint8_t*>(d), 1, PixelFormat::X2Bgr10, sum10);
    uint32_t out10[1];
    Average(sum10, 1, PixelFormat::X2Bgr10, 2, reinterpret_cast<uint8_t*>(out10));
    CHECK(out10[0] == pack(512, 500, 200, 3));

    // Only true black becomes green.
    uint32_t pixels[3] = { 0xFF000000u, 0xFF000001u, 0xFF102030u };
    PaintBlackGreen(reinterpret_cast<uint8_t*>(pixels), 3, PixelFormat::Bgra8);
    CHECK(pixels[0] == 0xFF00FF00u && pixels[1] == 0xFF000001u && pixels[2] == 0xFF102030u);
    uint32_t pixels10[2] = { 3u << 30, pack(1, 0, 0, 3) };
    PaintBlackGreen(reinterpret_cast<uint8_t*>(pixels10), 2, PixelFormat::X2Bgr10);
    CHECK(pixels10[0] == pack(0, 1023, 0, 3) && pixels10[1] == pack(1, 0, 0, 3));

    // Depth: whatever the surface colour, white fog minus black fog gives the same grey for the same fog amount.
    const auto fogged = [](double colour, double fog, double fogColour) { return static_cast<uint8_t>(colour * (1 - fog) + fogColour * fog + 0.5); };
    for (const double fog : { 0.0, 0.25, 0.5, 0.75, 1.0 }) {
        for (const double colour : { 0.0, 64.0, 200.0 }) {
            const uint8_t w = fogged(colour, fog, 255), k = fogged(colour, fog, 0);
            uint8_t white[4] = { w, w, w, 255 }, black[4] = { k, k, k, 255 };
            FogDifference(white, black, 1, PixelFormat::Bgra8);
            CHECK(std::abs(black[0] - fog * 255) <= 1.5 && black[0] == black[1] && black[1] == black[2] && black[3] == 255);
        }
    }
}

static void TestScanner()
{
    const std::vector<uint8_t> memory = { 0x10, 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x55, 0x00, 0x48, 0x8B, 0xFF };
    auto exact = scan::Parse("48 8B C4");
    CHECK(exact.has_value());
    CHECK(scan::FindAll(memory.data(), memory.size(), *exact).size() == 1);
    auto wildcard = scan::Parse("48 8B ??");
    CHECK(scan::FindAll(memory.data(), memory.size(), *wildcard).size() == 2);
    auto leading = scan::Parse("?? 58 18");
    const auto hits = scan::FindAll(memory.data(), memory.size(), *leading);
    CHECK(hits.size() == 1 && hits[0] == memory.data() + 5);
    CHECK(!scan::Parse("48 XYZ").has_value());
    CHECK(!scan::Parse("").has_value());
    CHECK(scan::FindAll(memory.data(), 2, *exact).empty());
}

static void TestIni()
{
    const fs::path path = fs::temp_directory_path() / "t8cine_test.ini";
    Ini out;
    out.Set("name", "hello world");
    out.SetNumber("speed", 0.25);
    out.SetBool("flag", true);
    CHECK(out.Save(path));
    Ini in;
    CHECK(in.Load(path));
    CHECK(in.Get("name") == "hello world");
    CHECK(Near(in.GetNumber("speed", 0), 0.25));
    CHECK(in.GetBool("flag", false));
    CHECK(in.Get("missing", "x") == "x");
    fs::remove(path);
}

static void TestVdf()
{
    const std::string libraries = R"("libraryfolders"
{
	"0"
	{
		"path"		"C:\\Program Files (x86)\\Steam"
		"apps" { "228980" "1" }
	}
	"1"
	{
		"path"		"D:\\steam"
	}
})";
    const auto paths = files::ParseLibraryFolders(libraries);
    CHECK(paths.size() == 2);
    CHECK(paths[0] == fs::path("C:\\Program Files (x86)\\Steam"));
    CHECK(paths[1] == fs::path("D:\\steam"));

    const std::string manifest = R"("AppState" { "appid" "1778820" "installdir" "TEKKEN 8" "StateFlags" "1026" "buildid" "0" })";
    CHECK(files::VdfValue(manifest, "installdir") == "TEKKEN 8");
    CHECK(files::VdfValue(manifest, "stateflags") == "1026");
    CHECK(files::VdfValue(manifest, "nope").empty());

    const std::string offline = R"("users"
{
	"111"
	{
		"AccountName"		"old"
		"MostRecent"		"0"
		"WantsOfflineMode"		"0"
	}
	"222"
	{
		"AccountName"		"me"
		"MostRecent"		"1"
		"WantsOfflineMode"		"1"
	}
})";
    CHECK(files::LoginUsersOffline(offline));
    std::string online = offline;
    online.replace(online.rfind("\"1\""), 3, "\"0\"");
    CHECK(!files::LoginUsersOffline(online));
    CHECK(!files::LoginUsersOffline(""));

    // How this Steam really writes it: no MostRecent at all, and only the registry says which account is signed in.
    const std::string current = R"("users"
{
	"111"
	{
		"AccountName"		"other"
		"AutoLogin"		"0"
		"WantsOfflineMode"		"0"
		"timestamp"		"100"
	}
	"222"
	{
		"AccountName"		"Mine"
		"AutoLogin"		"1"
		"WantsOfflineMode"		"1"
		"timestamp"		"50"
	}
})";
    CHECK(files::LoginUsersOffline(current, "mine"));
    CHECK(!files::LoginUsersOffline(current, "other"));
    CHECK(files::LoginUsersOffline(current)); // AutoLogin when the registry has nothing
    std::string noMarks = current;
    while (noMarks.find("\"AutoLogin\"") != std::string::npos)
        noMarks.replace(noMarks.find("\"AutoLogin\""), 11, "\"x\"");
    CHECK(!files::LoginUsersOffline(noMarks)); // the newest timestamp is the online account

    const std::string log = "[a] Current download rate: 500.000 Mbps\n"
                            "[b] AppID 1778820 update started : download 1000/2000000000, store 0/0\n"
                            "[c] Current download rate: 80.000 Mbps\n"
                            "[d] Current download rate: 40.000 Mbps\n";
    const auto estimate = files::EstimateDownload(log);
    CHECK(estimate.Known && estimate.Total == 2000000000);
    CHECK(estimate.Done == 1000 + 600000000 + 300000000); // the rate before the update started doesn't count
    CHECK(Near(estimate.Mbps, 40));
    CHECK(!files::EstimateDownload("nothing relevant").Known);
}

static void Touch(const fs::path& path, const std::string& contents)
{
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary | std::ios::trunc) << contents;
}

static void TestSessionFiles()
{
    const fs::path root = fs::temp_directory_path() / "t8cine_test_game";
    fs::remove_all(root);

    files::GameInstall game;
    game.Found = true;
    game.Root = root;
    game.Win64 = root / "Polaris" / "Binaries" / "Win64";
    game.Paks = root / "Polaris" / "Content" / "Paks";
    fs::create_directories(game.Win64);

    const fs::path built = root / "built" / "dinput8.dll";
    Touch(built, "binary T8CINE-BUILD-MARKER binary");
    const fs::path skinA = root / "skins" / "SkinA";
    Touch(skinA / "a_P.pak", "pak");
    Touch(skinA / "a_P.utoc", "toc");

    // A plain install, then a full removal leaves nothing behind.
    auto result = files::InstallMod(game, built, { skinA });
    CHECK(result.Ok);
    CHECK(fs::exists(game.Win64 / "dinput8.dll"));
    CHECK(fs::exists(game.Win64 / "T8Cine" / "session.lock"));
    CHECK(fs::exists(game.Paks / "~T8Cine" / "SkinA" / "a_P.pak"));
    CHECK(files::ModFilesPresent(game));
    result = files::RemoveMod(game);
    CHECK(result.Ok);
    CHECK(!fs::exists(game.Win64 / "dinput8.dll"));
    CHECK(!fs::exists(game.Win64 / "T8Cine" / "session.lock"));
    CHECK(!fs::exists(game.Paks / "~T8Cine"));
    CHECK(!files::ModFilesPresent(game));

    // Another mod's dinput8.dll is set aside and comes back byte for byte.
    Touch(game.Win64 / "dinput8.dll", "someone else's mod");
    result = files::InstallMod(game, built, {});
    CHECK(result.Ok);
    CHECK(files::IsOurDll(game.Win64 / "dinput8.dll"));
    CHECK(fs::exists(game.Win64 / "dinput8.dll.t8cine-backup"));
    result = files::RemoveMod(game);
    CHECK(result.Ok);
    CHECK(files::ReadTextFile(game.Win64 / "dinput8.dll") == "someone else's mod");
    CHECK(!fs::exists(game.Win64 / "dinput8.dll.t8cine-backup"));

    // Removal leaves a foreign DLL alone.
    result = files::RemoveMod(game);
    CHECK(result.Ok);
    CHECK(files::ReadTextFile(game.Win64 / "dinput8.dll") == "someone else's mod");

    // Other mods are reported and left alone; the game's own paks and our session folder aren't other mods.
    Touch(game.Paks / "pakchunk0-Windows.pak", "game");
    Touch(game.Paks / "mods" / "Old HUD_P.pak", "pak");
    Touch(game.Paks / "mods" / "Old HUD_P.utoc", "toc");
    Touch(game.Paks / "~T8Cine" / "Skin" / "skin_P.pak", "ours");
    auto others = files::OtherMods(game);
    CHECK(others.size() == 2 && others[0] == "Old HUD_P" && others[1].rfind("dinput8.dll", 0) == 0);
    fs::remove_all(game.Paks / "~T8Cine");
    CHECK(fs::exists(game.Paks / "mods" / "Old HUD_P.pak"));

    // Skins are listed with their sizes.
    const auto skins = files::ListSkins(root / "skins");
    CHECK(skins.size() == 1 && skins[0].Name == "SkinA" && skins[0].PakCount == 1 && skins[0].Bytes == 6);

    // Vault: snapshot, change the saves, restore, and the change is kept in an automatic backup.
    const fs::path saves = root / "saves";
    const fs::path vault = root / "vault";
    Touch(saves / "user" / "replay.sav", "first");

    // Saved replays are picked out by name, and other save files are left out.
    Touch(saves / "user" / "replay_game_no11.sav", "12345");
    Touch(saves / "user" / "replay_sys1.sav", "index");
    Touch(saves / "user" / "ghost_1_1.sav", "ghost");
    const auto replays = files::ListSavedReplays(saves);
    CHECK(replays.size() == 1 && replays[0].FileName == "replay_game_no11.sav" && replays[0].Bytes == 5);
    CHECK(files::ListSavedReplays(root / "not-there").empty());
    fs::remove(saves / "user" / "replay_game_no11.sav");
    fs::remove(saves / "user" / "replay_sys1.sav");
    fs::remove(saves / "user" / "ghost_1_1.sav");
    CHECK(files::SnapshotSaves(saves, vault, "before", "100").Ok);
    CHECK(!files::SnapshotSaves(saves, vault, "before", "100").Ok);
    Touch(saves / "user" / "replay.sav", "second");
    auto entries = files::ListVault(vault);
    CHECK(entries.size() == 1 && entries[0].BuildId == "100" && entries[0].Files == 1);
    result = files::RestoreSnapshot(saves, vault, entries[0], "200");
    CHECK(result.Ok);
    CHECK(result.Message.find("older game build") != std::string::npos);
    CHECK(files::ReadTextFile(saves / "user" / "replay.sav") == "first");
    entries = files::ListVault(vault);
    CHECK(entries.size() == 2);
    bool foundBackup = false;
    for (const auto& entry : entries)
        if (entry.Name.rfind("auto-backup-", 0) == 0)
            foundBackup = files::ReadTextFile(entry.Folder / "saves" / "user" / "replay.sav") == "second";
    CHECK(foundBackup);

    fs::remove_all(root);
}

int main()
{
    TestRotations();
    TestPath();
    TestRelativeCamera();
    TestSpeedRamp();
    TestLinearPath();
    TestLookAt();
    TestFrameMath();
    TestScanner();
    TestIni();
    TestVdf();
    TestSessionFiles();
    printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
