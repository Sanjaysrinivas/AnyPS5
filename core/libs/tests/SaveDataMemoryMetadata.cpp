#include "SceTypes.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>
#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
#endif

extern "C" {
int APS5_VABI sceSaveDataInitialize3(const void*);
int APS5_VABI sceSaveDataTerminate();
int APS5_VABI sceSaveDataSetSaveDataMemory2(const SaveDataMemorySet2*);
int APS5_VABI sceSaveDataSetupSaveDataMemory2(const SaveDataMemorySetup2*, SaveDataMemorySetupResult*);
}

static void Check(bool value, int line) {
    if (!value) {
        std::fprintf(stderr, "Save-data memory check failed at line %d\n", line);
        std::abort();
    }
}
#define Require(value) Check((value), __LINE__)

static std::vector<char> Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    Require(file.is_open());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

static std::vector<char> ParamBytes(const SaveDataParam& param) {
    const auto* bytes = reinterpret_cast<const char*>(&param);
    return {bytes, bytes + sizeof(param)};
}

static void ConfigureSetup(SaveDataMemorySetup2& setup, const SaveDataParam& init, std::uint32_t user, std::uint32_t slot,
                           std::size_t size) {
    setup.user_id = static_cast<std::int32_t>(user);
    setup.slot_id = slot;
    setup.memory_size = size;
    setup.option = 1;
    setup.init_param = &init;
}

static bool SetupRejected(SaveDataMemorySetup2& setup, SaveDataMemorySetupResult& result) {
    return sceSaveDataSetupSaveDataMemory2(&setup, &result) == static_cast<int>(0x809F000Bu);
}

static int SetupFails(SaveDataMemorySetup2& setup, SaveDataMemorySetupResult& result, const std::filesystem::path& bin,
                      const std::filesystem::path& param, bool blob_present) {
    std::filesystem::create_directories(param.parent_path());
    std::error_code remove_error;
    std::filesystem::remove(param.string() + ".tmp", remove_error);
    if (!std::filesystem::create_directory(param.string() + ".tmp")) {
        return 1;
    }
    result.existed_memory_size = 555;
    if (!SetupRejected(setup, result)) {
        return 2;
    }
    if (result.existed_memory_size != 555) {
        return 3;
    }
    if (std::filesystem::exists(param) || (!blob_present && std::filesystem::exists(bin))) {
        return 4;
    }
    if (std::filesystem::exists(param.string() + ".tmp")) {
        return 5;
    }
    return 0;
}

static void CheckSetupMetadataFailure() {
    const std::filesystem::path bin = "_sd_mem/u42/slot0.bin";
    const std::filesystem::path param = "_sd_mem/u42/slot0.param";
    SaveDataParam init{};
    init.user_param = 100;
    SaveDataMemorySetup2 setup{};
    ConfigureSetup(setup, init, 42, 0, 128);
    SaveDataMemorySetupResult result{};
    const int failure = SetupFails(setup, result, bin, param, false);
    Require(failure == 0);
    Require(sceSaveDataSetupSaveDataMemory2(&setup, &result) == 0);
    Require(result.existed_memory_size == 0);
    Require(Read(bin) == std::vector<char>(128, 0));
    Require(Read(param) == ParamBytes(init));
    std::remove(bin.string().c_str());
    std::remove(param.string().c_str());
}

static void CheckSetupMetadataFailureRetainsBlobSize() {
    const std::filesystem::path bin = "_sd_mem/u43/slot0.bin";
    const std::filesystem::path param = "_sd_mem/u43/slot0.param";
    std::filesystem::create_directories(bin.parent_path());
    const std::vector<char> original(16, 's');
    {
        std::ofstream file(bin, std::ios::binary);
        file.write(original.data(), static_cast<std::streamsize>(original.size()));
        Require(static_cast<bool>(file));
    }
    SaveDataParam init{};
    init.user_param = 77;
    SaveDataMemorySetup2 setup{};
    ConfigureSetup(setup, init, 43, 0, 32);
    SaveDataMemorySetupResult result{};
    const int failure = SetupFails(setup, result, bin, param, true);
    Require(failure == 0);
    Require(Read(bin) == original);
    Require(sceSaveDataSetupSaveDataMemory2(&setup, &result) == 0);
    Require(Read(bin).size() == 32);
    Require(Read(param) == ParamBytes(init));
    std::remove(bin.string().c_str());
    std::remove(param.string().c_str());
}

static void CheckSetupParamRecoveryPreservesSize() {
    const std::filesystem::path bin = "_sd_mem/u44/slot0.bin";
    const std::filesystem::path param = "_sd_mem/u44/slot0.param";
    std::filesystem::create_directories(bin.parent_path());
    std::vector<char> original(64);
    for (std::size_t index = 0; index < original.size(); ++index) {
        original[index] = static_cast<char>(index + 1);
    }
    {
        std::ofstream file(bin, std::ios::binary);
        file.write(original.data(), static_cast<std::streamsize>(original.size()));
        Require(static_cast<bool>(file));
    }
    SaveDataParam init{};
    init.user_param = 99;
    SaveDataMemorySetup2 setup{};
    ConfigureSetup(setup, init, 44, 0, 32);
    SaveDataMemorySetupResult result{};
    Require(sceSaveDataSetupSaveDataMemory2(&setup, &result) == 0);
    Require(result.existed_memory_size == original.size());
    Require(Read(bin) == original);
    Require(Read(param) == ParamBytes(init));
    setup.init_param = nullptr;
    Require(sceSaveDataSetupSaveDataMemory2(&setup, &result) == 0);
    Require(Read(bin) == original);
    Require(Read(param) == ParamBytes(init));
    std::remove(bin.string().c_str());
    std::remove(param.string().c_str());
}

static void CheckSetupWithoutParam() {
    SaveDataParam init{};
    init.user_param = 5;
    SaveDataMemorySetup2 setup{};
    SaveDataMemorySetupResult result{};
    ConfigureSetup(setup, init, 45, 0, 16);
    setup.init_param = nullptr;
    Require(sceSaveDataSetupSaveDataMemory2(&setup, &result) == 0);
    Require(Read("_sd_mem/u45/slot0.bin") == std::vector<char>(16, 0));
    Require(!std::filesystem::exists("_sd_mem/u45/slot0.param"));
    ConfigureSetup(setup, init, 46, 0, 16);
    setup.option = 0;
    Require(sceSaveDataSetupSaveDataMemory2(&setup, &result) == 0);
    Require(!std::filesystem::exists("_sd_mem/u46/slot0.param"));
    std::filesystem::remove_all("_sd_mem/u45");
    std::filesystem::remove_all("_sd_mem/u46");
}

static void CheckSetupSlotIsolation() {
    SaveDataParam first{};
    first.user_param = 1;
    SaveDataParam second{};
    second.user_param = 2;
    SaveDataMemorySetup2 setup{};
    SaveDataMemorySetupResult result{};
    ConfigureSetup(setup, first, 47, 0, 8);
    Require(sceSaveDataSetupSaveDataMemory2(&setup, &result) == 0);
    ConfigureSetup(setup, second, 47, 1, 8);
    Require(sceSaveDataSetupSaveDataMemory2(&setup, &result) == 0);
    Require(Read("_sd_mem/u47/slot0.param") == ParamBytes(first));
    Require(Read("_sd_mem/u47/slot1.param") == ParamBytes(second));
    ConfigureSetup(setup, second, 48, 0, 8);
    Require(sceSaveDataSetupSaveDataMemory2(&setup, &result) == 0);
    Require(Read("_sd_mem/u47/slot0.param") == ParamBytes(first));
    Require(Read("_sd_mem/u48/slot0.param") == ParamBytes(second));
    std::remove("_sd_mem/u47/slot0.bin");
    std::remove("_sd_mem/u47/slot0.param");
    std::remove("_sd_mem/u47/slot1.bin");
    std::remove("_sd_mem/u47/slot1.param");
    std::remove("_sd_mem/u48/slot0.bin");
    std::remove("_sd_mem/u48/slot0.param");
}

int main() {
    const auto previous = std::filesystem::current_path();
    const auto root = std::filesystem::temp_directory_path() /
        ("anyps5-metadata-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Require(std::filesystem::create_directory(root));
    std::filesystem::current_path(root);
    const auto path = std::filesystem::path("_sd_mem/u7531/slot0.param");
    const auto memoryPath = std::filesystem::path("_sd_mem/u7531/slot0.bin");
    std::filesystem::create_directories(path.parent_path());
    const std::vector<char> memoryOriginal(32, 's');
    {
        std::ofstream file(memoryPath, std::ios::binary);
        file.write(memoryOriginal.data(), static_cast<std::streamsize>(memoryOriginal.size()));
        Require(static_cast<bool>(file));
    }
    Require(sceSaveDataInitialize3(nullptr) == 0);
    Require(sceSaveDataInitialize3(nullptr) == 0);
    CheckSetupMetadataFailure();
    CheckSetupMetadataFailureRetainsBlobSize();
    CheckSetupParamRecoveryPreservesSize();
    CheckSetupWithoutParam();
    CheckSetupSlotIsolation();
    Require(sceSaveDataTerminate() == 0);
    SaveDataParam param{};
    SaveDataMemorySet2 set{};
    set.user_id = 7531;
    set.param = &param;
    Require(sceSaveDataSetSaveDataMemory2(&set) == 0);
    const auto original = Read(path);
    Require(original.size() == sizeof(param));
    param.user_param = 42;
    const auto temporary = path.string() + ".tmp";
    Require(std::filesystem::create_directory(temporary));
    const int status = sceSaveDataSetSaveDataMemory2(&set);
    Require(Read(path) == original);
    Require(status == static_cast<int>(0x809F000Bu));
    Require(!std::filesystem::exists(temporary));
    Require(sceSaveDataSetSaveDataMemory2(&set) == 0);
    const auto* bytes = reinterpret_cast<const char*>(&param);
    Require(Read(path) == std::vector<char>(bytes, bytes + sizeof(param)));
    set.param = nullptr;
    Require(sceSaveDataSetSaveDataMemory2(&set) == 0);
    std::array<char, 8> first{'a', '\0', 'b', 'c', 'd', 'e', 'f', 'g'};
    std::array<char, 8> second{'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o'};
    std::array<SaveDataMemoryData, 3> data{{
        {first.data(), first.size(), 4},
        {second.data(), second.size(), 24},
        {nullptr, 0, std::numeric_limits<std::size_t>::max()}
    }};
    set.data = data.data();
    set.data_num = static_cast<std::uint32_t>(data.size());
    data[1].offset = memoryOriginal.size();
    Require(sceSaveDataSetSaveDataMemory2(&set) == static_cast<int>(0x809F0000u));
    Require(Read(memoryPath) == memoryOriginal);
    data[1].offset = 24;
    const auto memoryTemporary = memoryPath.string() + ".tmp";
    Require(std::filesystem::create_directory(memoryTemporary));
    Require(sceSaveDataSetSaveDataMemory2(&set) == static_cast<int>(0x809F000Bu));
    Require(Read(memoryPath) == memoryOriginal);
    Require(!std::filesystem::exists(memoryTemporary));
#ifndef _WIN32
    rlimit previousLimit{};
    Require(getrlimit(RLIMIT_FSIZE, &previousLimit) == 0);
    auto writeLimit = previousLimit;
    writeLimit.rlim_cur = 16;
    const auto previousHandler = std::signal(SIGXFSZ, SIG_IGN);
    Require(previousHandler != SIG_ERR);
    Require(setrlimit(RLIMIT_FSIZE, &writeLimit) == 0);
    const int writeStatus = sceSaveDataSetSaveDataMemory2(&set);
    Require(setrlimit(RLIMIT_FSIZE, &previousLimit) == 0);
    Require(std::signal(SIGXFSZ, previousHandler) != SIG_ERR);
    Require(writeStatus == static_cast<int>(0x809F000Bu));
    Require(Read(memoryPath) == memoryOriginal);
    Require(!std::filesystem::exists(memoryTemporary));
#endif
    Require(sceSaveDataSetSaveDataMemory2(&set) == 0);
    auto memoryExpected = memoryOriginal;
    std::copy(first.begin(), first.end(), memoryExpected.begin() + 4);
    std::copy(second.begin(), second.end(), memoryExpected.begin() + 24);
    Require(Read(memoryPath) == memoryExpected);
    first.fill('z');
    set.data_num = 0;
    Require(sceSaveDataSetSaveDataMemory2(&set) == 0);
    std::copy(first.begin(), first.end(), memoryExpected.begin() + 4);
    Require(Read(memoryPath) == memoryExpected);
    Require(sceSaveDataTerminate() == 0);
    std::filesystem::current_path(previous);
    std::filesystem::remove_all(root);
}
