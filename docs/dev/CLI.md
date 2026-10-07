# Relinker command line

[`Cli::ParseArgs`](../../core/relinker/cli/src/CliArgs.cpp) returns an [`Args`](../../core/relinker/cli/include/Cli.hpp) value. [`main`](../../core/relinker/main.cpp) handles argument errors before reading an input file. [`Usage`](../user/USAGE.md) describes conversion options and runtime layout.

## Help

`--help` and `-h` set `Args::showHelp`. The parser still scans all arguments and rejects unknown options, missing option values, duplicate filter levels and extra positional arguments. Help does not require input/output paths or conversion-only option combinations.

`main` prints `Cli::HelpText()` to stdout and returns 0 before accessing files, constructing conversion stages, writing a registry or launching an output executable. Missing input/output paths without help retain the compact usage error on stderr and return 1.

Examples:

```sh
relinker --help
relinker -h
relinker --windows --help
```

## Exit behavior

| Request | Exit code | Output |
|---------|-----------|--------|
| Valid help request | 0 | Help on stdout |
| Invalid argument syntax, including with help | 1 | Error on stderr |
| Missing input/output without help | 1 | Usage on stderr |
| Conversion failure | 2 | Error on stderr |
| Successful conversion | 0 | Conversion log |
| Successful conversion with `--autorun` | Application exit code | Conversion and launch log |

## Verification

With `BUILD_TESTING=ON` and Python available, `cli_help` invokes the actual relinker in an empty temporary directory. It checks both help aliases, help combined with conversion options, argument failures, conversion failure and absence of output files.

```sh
ctest --test-dir build -R '^cli_help$' --output-on-failure
```
