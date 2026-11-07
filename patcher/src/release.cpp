// The release tool (maintainers only; macOS with Xcode). Two things the patcher uses can
// only be made on a Mac, and go into the release's files/ folder:
//   - ftlios.dylib, built from src/ with the iOS SDK;
//   - the prepatches: recorded by running the game once as a Mac Catalyst app (in record
//     mode, src/prepatch.c), then turned into templates without any bytes of the game.
//     That needs your decrypted IPA, and Hyperspace's macOS build from the same commit as
//     its iOS build.
// Then the patcher app is built for macOS, Windows (mingw-w64) and Linux (Docker), each
// zipped with files/ into dist/. Hyperspace isn't in the zips: its iOS build is signed and
// put into dist/ on its own, to hand out separately.
//
//   ftl-ios-release --ipa FTL.ipa --version 1.0.0 \
//       --hyperspace-ios <device build>/Hyperspace.1.6.21.arm64.dylib \
//       --hyperspace-mac <macOS build>/Hyperspace.1.6.21.arm64.dylib \
//       [--hyperspace-sim <simulator build>/Hyperspace.1.6.21.arm64.dylib] [--no-apps]
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <map>
#include <thread>

#include "macho.h"
#include "patch.h"
#include "plist.h"
#include "prepatch.h"

extern char **environ;

using namespace ftl;

namespace
{

const fs::path ROOT = fs::weakly_canonical(FTL_SOURCE_ROOT);  // the repository (CMake sets it)
const char *SOURCES[] = {"hooks.c", "png.c", "hyperspace.c", "prepatch.c", "display.c"};
const char *IOS_SOURCES[] = {"ios.m", "settings.m", "touch.m",
                             "gltex.c"};  // scenes, saves, logs; settings; touch; big textures
const uint32_t MACCATALYST_PLATFORM = 6;

struct Result
{
    int status;
    std::string output;  // stdout and stderr
};

[[noreturn]] void Die(const std::string &message)
{
    std::fprintf(stderr, "error: %s\n", message.c_str());
    std::exit(1);
}

std::string Join(const std::vector<std::string> &args)
{
    std::string s;
    for (auto &a : args) s += (s.empty() ? "" : " ") + a;
    return s;
}

// Runs a program and collects its output; with a timeout (seconds), it's stopped after that
Result Run(const std::vector<std::string> &args, const fs::path &cwd = {},
           const std::map<std::string, std::string> &env = {}, int timeout = 0)
{
    int pipeFds[2];
    if (pipe(pipeFds)) Die("pipe failed");
    std::vector<std::string> envStrings;
    for (char **e = environ; *e; e++) envStrings.push_back(*e);
    for (auto &[k, v] : env) envStrings.push_back(k + "=" + v);
    pid_t pid = fork();
    if (pid == 0)
    {
        dup2(pipeFds[1], 1);
        dup2(pipeFds[1], 2);
        close(pipeFds[0]);
        close(pipeFds[1]);
        if (!cwd.empty() && chdir(cwd.c_str())) _exit(127);
        std::vector<char *> argv, envp;
        for (auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);
        for (auto &e : envStrings) envp.push_back(const_cast<char *>(e.c_str()));
        envp.push_back(nullptr);
        environ = envp.data();
        execvp(args[0].c_str(), argv.data());
        _exit(127);
    }
    close(pipeFds[1]);
    std::atomic<bool> done{false};
    std::thread killer;
    if (timeout > 0)
        killer = std::thread(
            [&]
            {
                for (int i = 0; i < timeout * 10 && !done; i++)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (!done) kill(pid, SIGKILL);
            });
    Result r{0, ""};
    char buf[4096];
    ssize_t n;
    while ((n = read(pipeFds[0], buf, sizeof buf)) > 0) r.output.append(buf, size_t(n));
    close(pipeFds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    done = true;
    if (killer.joinable()) killer.join();
    r.status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return r;
}

// Runs it and stops the release if it fails
std::string Must(const std::vector<std::string> &args, const fs::path &cwd = {})
{
    Result r = Run(args, cwd);
    if (r.status)
    {
        std::fputs(r.output.c_str(), stderr);
        Die("failed: " + Join(args));
    }
    return r.output;
}

std::string Trim(std::string s)
{
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

// ftlios.dylib. The Catalyst copy that records the prepatch makes the same changes to the
// game (FTL_IOS) but leaves out the runtime-only iOS sources.
void BuildDylib(const std::string &sdk, const std::string &target, const fs::path &out, bool iosSources = true)
{
    std::string sdkPath = Trim(Must({"xcrun", "--sdk", sdk, "--show-sdk-path"}));
    std::vector<std::string> args = {"xcrun", "clang", "-target", target, "-isysroot", sdkPath};
    if (sdk == "macosx")
    {
        fs::path support = fs::path(sdkPath) / "System/iOSSupport";
        args.insert(args.end(), {"-iframework", (support / "System/Library/Frameworks").string(), "-isystem",
                                 (support / "usr/include").string()});
    }
    args.insert(args.end(), {"-O2", "-Wall", "-DFTL_IOS", "-fobjc-arc", "-framework", "CoreFoundation", "-framework",
                             "Foundation", "-framework", "UIKit"});
    if (iosSources)
        args.insert(args.end(), {"-framework", "UniformTypeIdentifiers", "-framework", "OpenGLES", "-framework",
                                 "QuartzCore", "-framework", "CoreGraphics", "-lz"});
    else
        args.push_back("-Wl,-U,_IosStartLog");  // hooks.c's weak call into ios.m, left out here
    args.insert(args.end(), {"-dynamiclib", "-install_name", std::string("@executable_path/Frameworks/") + DYLIB_NAME});
    for (const char *s : SOURCES) args.push_back((ROOT / "src" / s).string());
    if (iosSources)
        for (const char *s : IOS_SOURCES) args.push_back((ROOT / "src" / s).string());
    args.insert(args.end(), {"-o", out.string()});
    Must(args);
    Must({"codesign", "-f", "-s", "-", out.string()});
}

// Runs the prepared binary as a Mac Catalyst app in record mode; returns the recording
fs::path Record(const fs::path &work, const Bytes &data, const fs::path &hyperspaceMac)
{
    fs::path rec = work / "record" / "FTL.app";
    fs::remove_all(rec.parent_path());
    fs::create_directories(rec / "Frameworks");
    Bytes exe(data);
    macho::SetBuildVersion(exe, MACCATALYST_PLATFORM, "14.0", "14.0");
    WriteFile(rec / "FTL", exe);
    fs::permissions(rec / "FTL", fs::perms(0755));
    BuildDylib("macosx", "arm64-apple-ios14.0-macabi", rec / "Frameworks" / DYLIB_NAME, false);
    if (!hyperspaceMac.empty())
    {
        fs::copy_file(hyperspaceMac, rec / "Frameworks" / HYPERSPACE_NAME, fs::copy_options::overwrite_existing);
        Must({"codesign", "-f", "-s", "-", (rec / "Frameworks" / HYPERSPACE_NAME).string()});
    }
    std::string info = plist::ToXml(plist::Dict{{"CFBundleExecutable", "FTL"},
                                                {"CFBundleIdentifier", "com.ftlgame.FTL.prepatch"},
                                                {"CFBundlePackageType", "APPL"}});
    WriteFile(rec / "Info.plist", Bytes(info.begin(), info.end()));
    Must({"codesign", "-f", "-s", "-", rec.string()});
    fs::path out = work / "recording.bin";
    fs::remove(out);
    Result r = Run({(rec / "FTL").string()}, {}, {{"FTL_PREPATCH_RECORD", out.string()}}, 180);
    size_t start = 0;
    while (start < r.output.size())
    {
        size_t end = r.output.find('\n', start);
        std::string line = r.output.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (line.find("[ftlios]") != std::string::npos || line.find("[hyperspace]") != std::string::npos)
            std::printf("    %s\n", line.c_str());
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (r.status || !fs::exists(out)) Die("recording the prepatch failed (see above)");
    return out;
}

// The template filled from the game must give exactly what was recorded
void Check(const fs::path &tmpl, const prepatch::Recording &rec, const Bytes &game, const Bytes &data)
{
    Bytes expected(data);
    uint64_t textOff = macho::Segments(expected)["__TEXT"].fileoff;
    for (auto &[offset, value] : rec.words) Wr32(&expected[textOff + offset], value);
    uint64_t hs = macho::Segments(expected)["__HSTEXT"].fileoff;
    std::copy(rec.code.begin(), rec.code.end(), expected.begin() + int64_t(hs));
    Bytes got(data);
    prepatch::Apply(got, tmpl, game);
    if (got != expected) Die(U8Str(tmpl.filename()) + ": filling the template doesn't give the recording");
}

void CopyTree(const fs::path &from, const fs::path &to)
{
    fs::create_directories(to.parent_path());
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::copy_symlinks);
}

// The patcher program for macOS, Windows and Linux, each zipped with the release's files
void BuildApps(const std::string &version, const fs::path &files, const fs::path &dist)
{
    fs::path src = ROOT / "patcher";
    fs::path stage = fs::temp_directory_path() / ("ftl-apps-" + std::to_string(getpid()));
    fs::remove_all(stage);
    fs::create_directories(stage);

    std::printf("==> Patcher app: macOS (arm64 and x86_64)\n");
    Must({"cmake", "-S", src.string(), "-B", (src / "build-macos").string(), "-G", "Ninja",
          "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64",
          "-DCMAKE_OSX_DEPLOYMENT_TARGET=11.0"});
    Must({"cmake", "--build", (src / "build-macos").string(), "--target", "ftl-ios-patcher"});
    fs::path mac = stage / ("FTL-iOS-Patcher-" + version + "-macOS");
    fs::create_directories(mac);
    // Just the program, without an app bundle: like on Windows and Linux, files/ sits next to it
    fs::copy_file(src / "build-macos" / "FTL iOS Patcher.app" / "Contents" / "MacOS" / "FTL iOS Patcher",
                  mac / "FTL iOS Patcher");
    Must({"codesign", "--force", "-s", "-", (mac / "FTL iOS Patcher").string()});

    std::printf("==> Patcher app: Windows (x86_64, cross-compiled with mingw-w64)\n");
    Must({"cmake", "-S", src.string(), "-B", (src / "build-windows").string(), "-G", "Ninja",
          "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_TOOLCHAIN_FILE=" + (src / "cmake" / "mingw-w64.cmake").string()});
    Must({"cmake", "--build", (src / "build-windows").string(), "--target", "ftl-ios-patcher"});
    fs::path win = stage / ("FTL-iOS-Patcher-" + version + "-Windows");
    fs::create_directories(win);
    fs::copy_file(src / "build-windows" / "FTL iOS Patcher.exe", win / "FTL iOS Patcher.exe");

    std::printf("==> Patcher app: Linux (x86_64, built in Docker)\n");
    Must({(src / "build-linux.sh").string()});
    fs::path lin = stage / ("FTL-iOS-Patcher-" + version + "-Linux");
    fs::create_directories(lin);
    fs::copy_file(src / "build-linux" / "ftl-ios-patcher", lin / "ftl-ios-patcher");

    for (const fs::path &folder : {mac, win, lin})
    {
        CopyTree(files, folder / "files");
        fs::path zip = dist / (folder.filename().string() + ".zip");
        fs::remove(zip);
        Must({"zip", "-qry", zip.string(), folder.filename().string()}, stage); // keeps the programs executable
        std::printf("    %s\n", U8Str(zip.filename()).c_str());
    }
    fs::remove_all(stage);
}

int Usage()
{
    std::fprintf(
        stderr, "usage: ftl-ios-release --ipa FTL.ipa --version VERSION --hyperspace-ios DYLIB --hyperspace-mac DYLIB\n"
                "                       [--hyperspace-sim DYLIB] [--no-apps]\n");
    return 2;
}

} // namespace

int main(int argc, char **argv)
{
    std::map<std::string, fs::path> paths;
    std::string version;
    bool apps = true;
    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        if (a == "--no-apps")
            apps = false;
        else if (i + 1 >= argc)
            return Usage();
        else if (a == "--version")
            version = argv[++i];
        else if (a == "--ipa" || a == "--hyperspace-ios" || a == "--hyperspace-mac" || a == "--hyperspace-sim")
            paths[a] = fs::absolute(argv[++i]);
        else
            return Usage();
    }
    if (version.empty() || !paths.count("--ipa") || !paths.count("--hyperspace-ios") ||
        !paths.count("--hyperspace-mac"))
        return Usage();
    for (auto &[flag, p] : paths)
        if (!fs::is_regular_file(p)) Die(flag + ": not found: " + p.string());
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    try
    {
        fs::path files = ROOT / "files";
        fs::remove_all(files);
        fs::create_directories(files);
        fs::path work = fs::temp_directory_path() / ("ftl-release-" + std::to_string(getpid()));
        fs::remove_all(work);
        fs::create_directories(work);

        std::printf("==> Building ftlios.dylib\n");
        BuildDylib("iphoneos", "arm64-apple-ios14.0", files / DYLIB_NAME);

        Unpack(paths["--ipa"], work / "game", [](const std::string &) {});
        auto [game, data] = PrepareGame(work / "game");
        for (auto [name, hyperspace] : {std::pair<const char *, fs::path>{"prepatch.ftlt", {}},
                                        {"prepatch-hyperspace.ftlt", paths["--hyperspace-mac"]}})
        {
            std::printf("==> Recording %s\n", name);
            auto rec = prepatch::ReadRecording(Record(work, data, hyperspace));
            auto [nwords, nrefs] = prepatch::MakeTemplate(rec, game, data, files / name);
            Check(files / name, rec, game, data);
            std::printf("    %zu words, %zu references to the game, checked\n", nwords, nrefs);
        }

        if (paths.count("--hyperspace-sim"))
        {
            std::printf("==> Simulator files\n");
            fs::path sim = ROOT / "files-simulator";
            fs::remove_all(sim);
            fs::create_directories(sim);
            BuildDylib("iphonesimulator", "arm64-apple-ios14.0-simulator", sim / DYLIB_NAME);
            fs::copy_file(paths["--hyperspace-sim"], sim / HYPERSPACE_NAME);
            Must({"codesign", "-f", "-s", "-", (sim / HYPERSPACE_NAME).string()});
            for (auto &e : fs::directory_iterator(files))
                if (e.path().extension() == ".ftlt") fs::copy_file(e.path(), sim / e.path().filename());
        }
        fs::remove_all(work);

        fs::path dist = ROOT / "dist";
        fs::create_directories(dist);
        fs::path hsOut = dist / HYPERSPACE_NAME; // handed out on its own, not in the zips
        fs::copy_file(paths["--hyperspace-ios"], hsOut, fs::copy_options::overwrite_existing);
        Must({"codesign", "-f", "-s", "-", hsOut.string()});
        if (apps) BuildApps(version, files, dist);
        std::printf("Done: %s\n", files.c_str());
    }
    catch (const std::exception &e)
    {
        Die(e.what());
    }
    return 0;
}
