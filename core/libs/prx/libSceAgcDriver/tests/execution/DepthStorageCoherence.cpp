#include "DepthFastClearHarness.hpp"
#include "VulkanTestDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/DepthSurface.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#ifdef _WIN32
#include "prx/libc/include/GuestAllocations.hpp"
#include "prx/libc/include/GuestArena.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <new>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace DepthFastClearHarness;
constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Texels = Width * Height;
constexpr std::uint8_t Reference = 0x5a;
constexpr std::uint8_t Stored = 42;

struct Planes {
    alignas(4096) std::array<float, Texels * 2> depth{};
    alignas(4096) std::array<std::uint8_t, Texels * 2> stencil{};
    alignas(256) std::array<std::byte, HtileBytes> htile{};
};

std::array<Planes, 4> planes;
alignas(256) std::array<std::uint32_t, Texels * 4> outputMemory{};
std::span<std::uint32_t> output{outputMemory};

constexpr std::array<std::uint32_t, 17> loadCode{
    0x7e080218u, 0x343c0885u, 0x4a3c3d00u, 0x34063c84u, 0x2c3e3c86u, 0x363c3cffu, 0x0000003fu, 0x7e140280u,
    0x7e160280u, 0x7e180280u, 0x7e1a0280u, 0xf0001108u, 0x00010a1eu, 0xbf8c3f70u, 0xe0781000u, 0x80000a03u,
    0xbf810000u,
};

constexpr std::array<std::uint32_t, 15> storeCode{
    0x7e080218u, 0x343c0885u, 0x4a3c3d00u, 0x2c3e3c86u, 0x363c3cffu, 0x0000003fu, 0x7e1402aau, 0x7e160280u,
    0x7e180280u, 0x7e1a0280u, 0xf0201108u, 0x00010a1eu, 0xbf8c3f70u, 0xbf800000u, 0xbf810000u,
};

struct Program {
    std::vector<std::uint32_t> code;
    ShaderRecompiler::RecompileResult shader;
};

enum class Access { Load, Store, ReadWrite, Atomic };

Surface Target(Planes& data) {
    return {reinterpret_cast<std::uintptr_t>(data.depth.data()), reinterpret_cast<std::uintptr_t>(data.stencil.data()),
        reinterpret_cast<std::uintptr_t>(data.htile.data()), true, VK_FORMAT_D32_SFLOAT_S8_UINT};
}

VkStencilOpState Equal(std::uint8_t value) {
    return {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL, 0xffu, 0u, value};
}

void Initialize(AgcDriver::VulkanDevice& device, const Shaders& shaders, const Surface& target, std::uint8_t value = Reference) {
    const VkStencilOpState replace{VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0xffu, 0xffu, value};
    Require(Draw(device, shaders, target, {.depthTest = true, .depthWrite = true, .depthCompare = VK_COMPARE_OP_ALWAYS, .stencilTest = true, .stencil = replace}) == Covered, "attachment positive control failed");
}

Program Compute(AgcDriver::VulkanDevice& device, const Surface& target, bool stencil, Access access, bool array = false) {
    const bool store = access == Access::Store;
    const bool readWrite = access == Access::ReadWrite;
    const bool atomic = access == Access::Atomic;
    Program program;
    if (store) program.code.assign(storeCode.begin(), storeCode.end());
    else program.code.assign(loadCode.begin(), loadCode.end());
    if (readWrite) program.code.insert(program.code.end() - 1, {0xf0201108u, 0x00010a1eu, 0xbf8c3f70u});
    if (atomic) *std::find(program.code.begin(), program.code.end(), 0xf0001108u) = 0xf0642108u;
    if (array) {
        auto image = std::find(program.code.begin(), program.code.end(), store ? 0xf0201108u : 0xf0001108u);
        Require(image != program.code.end(), "array fixture lost its image instruction");
        *image += 0x20u;
        program.code.insert(image, 0x7e400280u);
    }
    if (store && !stencil) {
        const auto value = std::find(program.code.begin(), program.code.end(), 0x7e1402aau);
        Require(value != program.code.end(), "depth fixture lost its stored value");
        *value = 0x7e1402ffu;
        program.code.insert(value + 1, 0x3e800000u);
    }
    const auto address = stencil ? target.stencil : target.depth;
    const std::array<std::uint32_t, 8> texture{
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | ((stencil ? 5u : store || atomic ? 20u : 22u) << 20u) | (((Width - 1u) & 3u) << 30u),
        ((Width - 1u) >> 2u) | ((Height - 1u) << 14u), 0x24cu | ((array ? 13u : 9u) << 28u), 0u, 0u, 0u, 0u
    };
    const auto outputAddress = reinterpret_cast<std::uintptr_t>(output.data());
    const std::array<std::uint32_t, 4> buffer{static_cast<std::uint32_t>(outputAddress), static_cast<std::uint32_t>((outputAddress >> 32u) & 0xffffu), static_cast<std::uint32_t>(output.size() * 4u), 0x31016facu};
    std::vector<std::uint32_t> userData(24, 0u);
    std::copy(buffer.begin(), buffer.end(), userData.begin());
    std::copy(texture.begin(), texture.end(), userData.begin() + 4);
    const auto code = std::span<const std::uint32_t>(program.code);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {true, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderRecompiler::ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory}, device.Target(), {0, 0, 0, 128}
    };
    request.useCache = false;
    program.shader = ShaderRecompiler::Recompile(request);
    if (atomic) Require(std::none_of(program.shader.bindings.begin(), program.shader.bindings.end(), [](const auto& binding) { return binding.kind == ShaderRecompiler::DescriptorKind::SampledImage; }), "atomic fixture unexpectedly uses sampled images");
    return program;
}

void Run(AgcDriver::VulkanDevice& device, const Program& program, bool wait = true, std::shared_ptr<const AgcDriver::Recipe>* recipe = nullptr) {
    std::fill(output.begin(), output.end(), 0xdeadbeefu);
    device.Dispatch(program.shader, Texels / Threads, 1, 1, {}, reinterpret_cast<std::uintptr_t>(program.code.data()), nullptr, recipe);
    if (wait) device.WaitIdle();
}

void Read(AgcDriver::VulkanDevice& device, const Program& program, std::uint32_t expected) {
    Run(device, program);
    for (std::uint32_t texel = 0; texel < Texels; ++texel) {
        Require(output[texel * 4u] == expected, "storage read returned " + std::to_string(output[texel * 4u]) + " at texel " + std::to_string(texel) + ", expected " + std::to_string(expected));
    }
}

void DepthClear(AgcDriver::VulkanDevice& device, const Shaders& shaders) {
    const auto target = Target(planes[0]);
    Initialize(device, shaders, target);
    const auto store = Compute(device, target, true, Access::Store);
    Run(device, store);
    AgcDriver::Graphics::NoteDepthMetadataFill(target.htile, HtileBytes, 0x300u);
    Require(Draw(device, shaders, target, {.depthTest = false, .stencilTest = true, .stencil = Equal(Stored)}) == Covered, "depth-only fast clear discarded pending stencil storage writes");
    Require(Draw(device, shaders, target, {}) == Covered, "depth-only fast clear did not clear depth to one");
    Read(device, Compute(device, target, true, Access::Load), Stored);
}

void StencilClear(AgcDriver::VulkanDevice& device, const Shaders& shaders) {
    const auto target = Target(planes[1]);
    Initialize(device, shaders, target);
    Run(device, Compute(device, target, false, Access::Store));
    AgcDriver::Graphics::NoteDepthMetadataFill(target.htile, HtileBytes, 0xfu);
    Require(Draw(device, shaders, target, {.depthCompare = VK_COMPARE_OP_GREATER}) == Covered, "stencil-only fast clear discarded pending depth storage writes");
    Require(Draw(device, shaders, target, {.depthTest = false, .stencilTest = true, .stencil = Equal(0)}) == Covered, "stencil-only fast clear did not clear stencil to zero");
    Read(device, Compute(device, target, false, Access::Load), 0x3e800000u);
}

void CachedRead(AgcDriver::VulkanDevice& device, const Shaders& shaders, bool readWrite = false) {
    const auto target = Target(planes[2]);
    Initialize(device, shaders, target);
    const auto read = Compute(device, target, true, readWrite ? Access::ReadWrite : Access::Load);
    Read(device, read, Reference);
    Initialize(device, shaders, target, Stored);
    Read(device, read, Stored);
    Read(device, read, Stored);
}

void Array(AgcDriver::VulkanDevice& device, const Shaders& shaders) {
    const auto target = Target(planes[3]);
    Initialize(device, shaders, target);
    Read(device, Compute(device, target, true, Access::Load, true), Reference);
    Run(device, Compute(device, target, true, Access::Store, true));
    Read(device, Compute(device, target, true, Access::ReadWrite), Stored);
    Require(Draw(device, shaders, target, {.depthTest = false, .stencilTest = true, .stencil = Equal(Stored)}) == Covered, "one-layer array storage writes did not reach stencil attachment");
    Read(device, Compute(device, target, false, Access::Load), 0x3f000000u);
}

void QueuedRemoval(AgcDriver::VulkanDevice& device, const Shaders& shaders) {
    const auto target = Target(planes[0]);
    Initialize(device, shaders, target);
    const auto read = Compute(device, target, true, Access::ReadWrite);
    Run(device, read, false);
    AgcDriver::Graphics::ClearDepthSurfaces(device.Device());
    device.WaitIdle();
    for (std::uint32_t texel = 0; texel < Texels; ++texel) {
        Require(output[texel * 4u] == Reference, "queued storage read lost its resident image during cache removal");
    }
}

void ClearSeed(AgcDriver::VulkanDevice& device, const Shaders& shaders) {
    const auto target = Target(planes[1]);
    Initialize(device, shaders, target);
    const auto read = Compute(device, target, true, Access::ReadWrite);
    Read(device, read, Reference);
    AgcDriver::Graphics::NoteDepthMetadataFill(target.htile, HtileBytes, 0xfu);
    Read(device, read, 0u);
    Read(device, Compute(device, target, false, Access::Load), 0x3f000000u);
}

void AtomicClear(AgcDriver::VulkanDevice& device, const Shaders& shaders) {
    const auto target = Target(planes[1]);
    Initialize(device, shaders, target);
    const auto read = Compute(device, target, false, Access::Atomic);
    Read(device, read, 0x3f000000u);
    AgcDriver::Graphics::NoteDepthMetadataFill(target.htile, HtileBytes, 0x300u);
    Read(device, read, 0x3f800000u);
    Initialize(device, shaders, target);
    Read(device, read, 0x3f000000u);
}

#ifdef _WIN32
void TrackedStorage(AgcDriver::VulkanDevice& device, const Shaders& shaders) {
    constexpr std::size_t Block = 65536;
    auto* memory = GuestArena::GuestArenaAllocate_nid_postfix(2 * Block, Block);
    GuestArena::GuestArenaCommit_nid_postfix(memory, 2 * Block, PAGE_READWRITE, 2 * Block);
    {
        GuestAllocations::Mutation mutation;
        mutation.Add(memory, 2 * Block, true, true);
    }
    const auto release = [&] {
        device.WaitIdle();
        AgcDriver::Graphics::ClearCachedTextures(device.Device());
        AgcDriver::Graphics::ClearDepthSurfaces(device.Device());
        output = outputMemory;
        GuestAllocations::Mutation mutation;
        mutation.Remove(memory);
        GuestArena::GuestArenaReset_nid_postfix(memory, 2 * Block);
        GuestArena::GuestArenaRelease_nid_postfix(memory, 2 * Block);
    };
    try {
        auto* data = new (memory) Planes{};
        output = {reinterpret_cast<std::uint32_t*>(static_cast<std::byte*>(memory) + Block), Texels * 4};
        const auto target = Target(*data);
        Initialize(device, shaders, target);
        const auto read = Compute(device, target, false, Access::Atomic);
        std::shared_ptr<const AgcDriver::Recipe> recipe;
        Run(device, read, true, &recipe);
        Require(recipe != nullptr, "tracked storage fixture did not create reusable dispatch resources");
        AgcDriver::Graphics::NoteDepthMetadataFill(target.htile, HtileBytes, 0x300u);
        Read(device, read, 0x3f800000u);
        Initialize(device, shaders, target);
        Read(device, read, 0x3f000000u);
        release();
    } catch (...) {
        release();
        throw;
    }
}
#endif

}

int main(int argc, char** argv) {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const auto shaders = Compile(*device);
        const std::string scenario = argc > 1 ? argv[1] : "all";
        if (scenario == "all" || scenario == "depth_clear") DepthClear(*device, shaders);
        if (scenario == "all" || scenario == "stencil_clear") StencilClear(*device, shaders);
        if (scenario == "all" || scenario == "cached_read") CachedRead(*device, shaders);
        if (scenario == "all" || scenario == "cached_storage") CachedRead(*device, shaders, true);
        if (scenario == "all" || scenario == "array") Array(*device, shaders);
        if (scenario == "all" || scenario == "clear_seed") ClearSeed(*device, shaders);
        if (scenario == "all" || scenario == "atomic_clear") AtomicClear(*device, shaders);
        if (scenario == "all" || scenario == "queued") QueuedRemoval(*device, shaders);
#ifdef _WIN32
        if ((scenario == "all" && std::getenv("APS5_NO_TEXTURE_CACHE") == nullptr) || scenario == "tracked") TrackedStorage(*device, shaders);
#endif
        Require(scenario == "all" || scenario == "depth_clear" || scenario == "stencil_clear" || scenario == "cached_read" || scenario == "cached_storage" || scenario == "array" || scenario == "clear_seed" || scenario == "atomic_clear" || scenario == "queued" || scenario == "tracked", "unknown depth storage test scenario");
        std::cout << "depth storage coherence tests passed: " << scenario << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
