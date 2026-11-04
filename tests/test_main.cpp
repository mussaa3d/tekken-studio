// Checks that need no game: run tekken8mvm_tests.exe; it prints failures and exits non-zero if any.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "common/cinemath.h"
#include "common/config.h"
#include "common/framemath.h"
#include "common/guardlogic.h"
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
    RelativeCamera zoomed = rel;
    zoomed.Fov = rel.Fov - 30;
    CHECK(IsCinematicJump(rel, zoomed, 60, 8));
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

static void TestPathStyles()
{
    // A key can turn further than half a circle, and the path goes the way it was made.
    const std::vector<CameraKey> turn = { { 0, {}, { 0, 0, 0 }, 45 }, { 1, {}, { 0, 270, 0 }, 45 } };
    CHECK(Near(SamplePathLinear(turn, 0.5).Rotation.Yaw, 135));
    double last = 0;
    bool oneWay = true;
    for (const PathStyle style : { PathStyle::Smooth, PathStyle::Linear, PathStyle::CoD }) {
        last = 0;
        for (int i = 0; i <= 100; ++i) {
            const double t = i / 100.0;
            const double yaw = SamplePathStyled(turn, t, style).Rotation.Yaw;
            oneWay = oneWay && yaw >= last - 1e-6;
            last = yaw;
        }
    }
    CHECK(oneWay && Near(last, 270));

    // The curve styles pass through every key. CoD ignores key spacing, Smooth keeps its speed through it.
    const std::vector<CameraKey> keys = { { 0, { 0, 0, 0 }, {}, 45 }, { 1, { 100, 0, 0 }, {}, 45 }, { 5, { 200, 0, 0 }, {}, 45 }, { 6, { 300, 0, 0 }, {}, 45 } };
    for (const CameraKey& key : keys) {
        CHECK(Near(SamplePathStyled(keys, key.Time, PathStyle::CoD).Location.X, key.Location.X, 1e-6));
        CHECK(Near(SamplePathStyled(keys, key.Time, PathStyle::Smooth).Location.X, key.Location.X, 1e-6));
    }
    CHECK(Near(SamplePathStyled(keys, 3, PathStyle::CoD).Location.X, 150, 1e-6));

    // Old files stored yaw from -180 to 180. Loading them takes the short way across the seam.
    std::vector<CameraKey> old = { { 0, {}, { 0, 170, 0 }, 45 }, { 1, {}, { 0, -170, 0 }, 45 } };
    CountYaw(old);
    CHECK(Near(old[1].Rotation.Yaw, 190));
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

    // Dark and connected to the edge becomes green. Dark but walled in by a bright fighter stays.
    {
        std::vector<int> stack;
        const uint32_t dark = 0xFF050A0Bu, bright = 0xFF8090A0u, green = 0xFF00FF00u;
        uint32_t image[5 * 5] = {
            dark, dark,   dark,   dark,   dark,
            dark, bright, bright, bright, dark,
            dark, bright, dark,   bright, dark,
            dark, bright, bright, bright, dark,
            dark, dark,   dark,   dark,   dark,
        };
        PaintBackgroundKey(reinterpret_cast<uint8_t*>(image), 5, 5, PixelFormat::Bgra8, 20, false, stack);
        CHECK(image[0] == green && image[4] == green && image[24] == green && image[10] == green);
        CHECK(image[12] == dark);
        CHECK(image[6] == bright && image[18] == bright);

        uint32_t image10[2 * 2] = { pack(20, 40, 44, 3), pack(20, 40, 44, 3), pack(500, 500, 500, 3), pack(20, 40, 44, 3) };
        PaintBackgroundKey(reinterpret_cast<uint8_t*>(image10), 2, 2, PixelFormat::X2Bgr10, 20, false, stack);
        CHECK(image10[0] == pack(0, 1023, 0, 3) && image10[3] == pack(0, 1023, 0, 3));
        CHECK(image10[2] == pack(500, 500, 500, 3));

        uint32_t blue10[1] = { pack(20, 40, 44, 3) };
        PaintBackgroundKey(reinterpret_cast<uint8_t*>(blue10), 1, 1, PixelFormat::X2Bgr10, 20, true, stack);
        CHECK(blue10[0] == pack(0, 0, 1023, 3));
        uint32_t blue8[1] = { dark };
        PaintBackgroundKey(reinterpret_cast<uint8_t*>(blue8), 1, 1, PixelFormat::Bgra8, 20, true, stack);
        CHECK(blue8[0] == 0xFF0000FFu);
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

static void TestGuard()
{
    ConnectionAges ages;
    CHECK(ages.Update({ "a:80" }, 1000).count("a:80") == 1);
    CHECK(ages.OpenLongerThan(60000).empty());
    // Still open but not yet at the limit, and not reported as new again.
    CHECK(ages.Update({ "a:80" }, 30000).empty());
    CHECK(ages.OpenLongerThan(60000).empty());
    // A connection that closes and comes back starts over: repeated short checks never add up.
    ages.Update({}, 40000);
    ages.Update({ "a:80" }, 50000);
    ages.Update({ "a:80" }, 100000);
    CHECK(ages.OpenLongerThan(60000).empty());
    // One that stays open gets caught.
    ages.Update({ "a:80", "b:443" }, 111000);
    CHECK(ages.OpenLongerThan(60000) == "a:80");
    ages.Update({ "b:443" }, 112000);
    CHECK(ages.OpenLongerThan(60000).empty());
    ages.Update({ "b:443" }, 171000);
    CHECK(ages.OpenLongerThan(60000) == "b:443");
}

static void TestIni()
{
    const fs::path path = fs::temp_directory_path() / "tekken8mvm_test.ini";
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
    const fs::path root = fs::temp_directory_path() / "tekken8mvm_test_game";
    fs::remove_all(root);

    files::GameInstall game;
    game.Found = true;
    game.Root = root;
    game.Win64 = root / "Polaris" / "Binaries" / "Win64";
    game.Paks = root / "Polaris" / "Content" / "Paks";
    fs::create_directories(game.Win64);

    const fs::path built = root / "built" / "dinput8.dll";
    Touch(built, "binary TEKKEN8MVM-BUILD-MARKER binary");
    const fs::path skinA = root / "skins" / "SkinA";
    Touch(skinA / "a_P.pak", "pak");
    Touch(skinA / "a_P.utoc", "toc");

    // A plain install, then a full removal leaves nothing behind.
    auto result = files::InstallMod(game, built, { skinA });
    CHECK(result.Ok);
    CHECK(fs::exists(game.Win64 / "dinput8.dll"));
    CHECK(fs::exists(game.Win64 / "tekken8mvm" / "session.lock"));
    CHECK(fs::exists(game.Paks / "~tekken8mvm" / "SkinA" / "a_P.pak"));
    CHECK(files::ModFilesPresent(game));
    result = files::RemoveMod(game);
    CHECK(result.Ok);
    CHECK(!fs::exists(game.Win64 / "dinput8.dll"));
    CHECK(!fs::exists(game.Win64 / "tekken8mvm" / "session.lock"));
    CHECK(!fs::exists(game.Paks / "~tekken8mvm"));
    CHECK(!files::ModFilesPresent(game));

    // Another mod's dinput8.dll is set aside and comes back byte for byte.
    Touch(game.Win64 / "dinput8.dll", "someone else's mod");
    result = files::InstallMod(game, built, {});
    CHECK(result.Ok);
    CHECK(files::IsOurDll(game.Win64 / "dinput8.dll"));
    CHECK(fs::exists(game.Win64 / "dinput8.dll.tekken8mvm-backup"));
    result = files::RemoveMod(game);
    CHECK(result.Ok);
    CHECK(files::ReadTextFile(game.Win64 / "dinput8.dll") == "someone else's mod");
    CHECK(!fs::exists(game.Win64 / "dinput8.dll.tekken8mvm-backup"));

    // Removal leaves a foreign DLL alone.
    result = files::RemoveMod(game);
    CHECK(result.Ok);
    CHECK(files::ReadTextFile(game.Win64 / "dinput8.dll") == "someone else's mod");

    // Other mods are reported and left alone; the game's own paks and our session folder aren't other mods.
    Touch(game.Paks / "pakchunk0-Windows.pak", "game");
    Touch(game.Paks / "mods" / "Old HUD_P.pak", "pak");
    Touch(game.Paks / "mods" / "Old HUD_P.utoc", "toc");
    // Coming from the old name: settings and presets come along, leftover skins go.
    Touch(game.Win64 / "T8Cine" / "t8cine.ini", "old");
    Touch(game.Win64 / "T8Cine" / "presets" / "mine.t8look", "old");
    Touch(game.Paks / "~T8Cine" / "Skin" / "skin_P.pak", "old");
    fs::remove_all(game.Win64 / "tekken8mvm");
    MoveFromOldName(game);
    CHECK(fs::exists(game.Win64 / "tekken8mvm" / "tekken8mvm.ini") && fs::exists(game.Win64 / "tekken8mvm" / "presets" / "mine.t8look"));
    CHECK(!fs::exists(game.Win64 / "T8Cine") && !fs::exists(game.Paks / "~T8Cine"));

    Touch(game.Paks / "~tekken8mvm" / "Skin" / "skin_P.pak", "ours");
    auto others = files::OtherMods(game);
    CHECK(others.size() == 2 && others[0] == "Old HUD_P" && others[1].rfind("dinput8.dll", 0) == 0);
    fs::remove_all(game.Paks / "~tekken8mvm");
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
    TestPathStyles();
    TestLookAt();
    TestFrameMath();
    TestScanner();
    TestGuard();
    TestIni();
    TestVdf();
    TestSessionFiles();
    printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
