// FTL iOS Patcher: the window without arguments, a command line with them.
//
//   ftl-ios-patcher unpack FTL.ipa FOLDER
//   ftl-ios-patcher pack FOLDER OUT.ipa [--name NAME] [--icon IMAGE] [--bundle-id ID]
//                        [--hyperspace DYLIB] [--files DIR]
// --hyperspace: Hyperspace's iOS build, needed when the mods include Hyperspace (releases don't
// ship it); with its prepatch-hyperspace.ftlt next to it, if it has other hooks than the release's.
//
// Between the two, mods can be applied to FOLDER with ftlman.
#include <cstdio>
#include <iostream>

#include "patch.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

int GuiMain(int argc, char **argv);

static int Usage()
{
    std::fprintf(stderr,
                 "usage: ftl-ios-patcher                 (the window)\n"
                 "       ftl-ios-patcher unpack FTL.ipa FOLDER\n"
                 "       ftl-ios-patcher pack FOLDER OUT.ipa [--name NAME] [--icon IMAGE] [--bundle-id ID]\n"
                 "                [--hyperspace DYLIB] [--files DIR]\n"
                 "Between the two, apply mods to FOLDER with ftlman; Hyperspace is included when the mods have it,\n"
                 "and then needs its iOS build with --hyperspace.\n");
    return 2;
}

static int Cli(const std::vector<std::string> &args)
{
    using namespace ftl;
    Log log = [](const std::string &s)
    {
        std::printf("%s\n", s.c_str());
        std::fflush(stdout);
    };
    try
    {
        if (args.size() == 3 && args[0] == "unpack")
        {
            Unpack(U8Path(args[1]), U8Path(args[2]), log);
            return 0;
        }
        if (args.size() >= 3 && args[0] == "pack")
        {
            PackOptions opt;
            fs::path files = FindFiles();
            for (size_t i = 3; i < args.size(); i++)
            {
                const std::string &a = args[i];
                if (i + 1 >= args.size() && a != "--simulator") return Usage();
                if (a == "--name")
                    opt.name = args[++i];
                else if (a == "--icon")
                    opt.icon = U8Path(args[++i]);
                else if (a == "--bundle-id")
                    opt.bundleId = args[++i];
                else if (a == "--hyperspace")
                    opt.hyperspace = U8Path(args[++i]);
                else if (a == "--files")
                    files = U8Path(args[++i]);
                else if (a == "--simulator")
                    opt.simulator = true;
                else
                    return Usage();
            }
            Pack(U8Path(args[1]), files, U8Path(args[2]), opt, log);
            return 0;
        }
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return Usage();
}

#ifdef _WIN32
// A GUI program: arguments come as UTF-16; the console is the one it was started from, if any
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    int argc;
    wchar_t **wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) args.push_back(ftl::U8Str(std::filesystem::path(wargv[i])));
    LocalFree(wargv);
    if (args.empty() || args[0].rfind("-psn", 0) == 0) return GuiMain(0, nullptr);
    if (AttachConsole(ATTACH_PARENT_PROCESS))
    {
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
    }
    SetConsoleOutputCP(CP_UTF8);
    return Cli(args);
}
#else
int main(int argc, char **argv)
{
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || args[0].rfind("-psn", 0) == 0) return GuiMain(argc, argv);  // -psn: old macOS Finder launches
    return Cli(args);
}
#endif
