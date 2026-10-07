#include <Cli.hpp>
#include <iostream>
#include <stdexcept>
#include <string>

namespace Cli {

const char* HelpText() {
    return
        "Usage: relinker [options] <input.elf> <output>\n"
        "\n"
        "Options:\n"
        "  --help, -h               Print this help and exit.\n"
        "  --windows                Produce a Windows PE executable; default is Linux ELF.\n"
        "  --windows-diagnostics    Include startup dependency diagnostics; requires --windows.\n"
        "  --windows-gui            Use the Windows GUI subsystem; requires --windows.\n"
        "  --to-intel               Convert supported AMD-only instructions.\n"
        "  unused-filter=0|1|2      Select unused-import analysis; default is 0.\n"
        "  --registry               Write <output-stem>.registry.json beside the output.\n"
        "  --rpath <path>           Set the system library search path; default is $ORIGIN/libs.\n"
        "  --autorun                Run the converted output and wait for Enter.\n"
        "\n"
        "Deprecated debugging options:\n"
        "  --skip-syscall-check      Disable syscall scanning.\n"
        "  --skip-sce-module         Skip bundled modules.\n"
        "  --exclude-sce-module <file>  Exclude a bundled module by filename; repeatable.\n"
        "  --lazy-binding           Use lazy binding; incompatible with bundled modules.\n"
        "\n"
        "Examples:\n"
        "  relinker source/input.elf app.elf\n"
        "  relinker --windows source/input.elf app.exe\n"
        "  relinker --rpath '$ORIGIN/libs' source/input.elf app.elf\n";
}

Args ParseArgs(int argc, char* argv[]) {
    Args args;
    bool unusedFilterSpecified = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            args.showHelp = true;
        } else if (arg == "--skip-syscall-check") {
            args.skipSyscallCheck = true;
        } else if (arg == "--skip-sce-module") {
            args.skipSceModule = true;
        } else if (arg == "--exclude-sce-module") {
            if (i + 1 >= argc)
                throw std::runtime_error("--exclude-sce-module requires a file name");
            args.excludedSceModules.insert(argv[++i]);
        } else if (arg == "--to-intel") {
            args.toIntel = true;
        } else if (arg.rfind("unused-filter=", 0) == 0) {
            const std::string value = arg.substr(14);
            if (unusedFilterSpecified || value.size() != 1 || value[0] < '0' || value[0] > '2')
                throw std::runtime_error("unused-filter must be specified once with a value of 0, 1 or 2");
            args.unusedFilterLevel = static_cast<std::uint32_t>(value[0] - '0');
            unusedFilterSpecified = true;
        } else if (arg == "--registry") {
            args.writeRegistry = true;
        } else if (arg == "--rpath") {
            if (i + 1 >= argc)
                throw std::runtime_error("--rpath requires a value");
            args.runPath = argv[++i];
        } else if (arg == "--windows") {
            args.toWindows = true;
        } else if (arg == "--lazy-binding") {
            args.lazyBinding = true;
        } else if (arg == "--autorun") {
            args.autorun = true;
        } else if (arg == "--windows-diagnostics") {
            args.windowsDiagnostics = true;
        } else if (arg == "--windows-gui") {
            args.windowsGui = true;
        } else if (arg.rfind("--", 0) == 0 || arg == "unused-filter") {
            throw std::runtime_error("unknown option: " + arg);
        } else if (args.inputPath.empty()) {
            args.inputPath = arg;
        } else if (args.outputPath.empty()) {
            args.outputPath = arg;
        } else {
            throw std::runtime_error("unexpected argument: " + arg);
        }
    }

    if (args.showHelp) return args;

    if (args.skipSceModule && !args.excludedSceModules.empty())
        throw std::runtime_error("--exclude-sce-module conflicts with --skip-sce-module");

    if (args.windowsDiagnostics && !args.toWindows)
        throw std::runtime_error("--windows-diagnostics requires --windows");

    if (args.windowsGui && !args.toWindows)
        throw std::runtime_error("--windows-gui requires --windows");

    if (args.inputPath.empty() || args.outputPath.empty())
        throw std::runtime_error(
            "Usage: relinker [--windows] [--windows-diagnostics] [--windows-gui] [--skip-syscall-check] [--skip-sce-module] [--exclude-sce-module <file>]... [--to-intel] [unused-filter=0|1|2] [--registry] [--rpath <path>] [--lazy-binding] [--autorun] <input.elf> <output.elf>\n"
            "Example: relinker input.elf output.elf"
        );

    return args;
}

}
