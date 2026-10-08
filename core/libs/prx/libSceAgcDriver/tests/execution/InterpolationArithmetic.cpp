#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "SpirvBackend/SpirvEmitterInstructions.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>

namespace {

using namespace ShaderRecompiler;
using AgcDriver::Graphics::Require;

struct Vector {
    std::array<std::uint32_t, 3> input;
    std::uint32_t expected;
};

constexpr std::array<Vector, 6> Vectors{{
    {{0x00000000u, 0x7f800000u, 0x477fe000u}, 0xffc00000u},
    {{0xff800000u, 0x00000000u, 0xffc02000u}, 0xffc00000u},
    {{0x7f800000u, 0x7f7fffffu, 0xff800000u}, 0xffc00000u},
    {{0x00000000u, 0x7f800000u, 0x4f000000u}, 0xfe00u},
    {{0x80000000u, 0xff800000u, 0x7fc00000u}, 0xfe00u},
    {{0xffc02000u, 0x3f800000u, 0x47800000u}, 0xfe01u},
}};
constexpr std::uint32_t Sentinel = 0xdeadbeefu;
alignas(256) std::array<std::uint32_t, 28> Data{};

RecompileResult Compile() {
    IrProgram program;
    const ShaderStageInputInfo inputs{};
    SpirvEmitterState state(program, inputs);
    auto& module = state.module;
    module.EmitCapability(spv::CapabilityShader);
    module.EmitCapability(spv::CapabilityFloat64);
    module.EmitCapability(spv::CapabilitySignedZeroInfNanPreserve);
    module.EmitExtension("SPV_KHR_float_controls");
    module.AddMemoryModel(spv::AddressingModelLogical, spv::MemoryModelGLSL450);
    const auto u32 = TypeU32(state);
    const auto array = module.DecoratedType(spv::OpTypeRuntimeArray, {{spv::OpDecorate, {spv::DecorationArrayStride, 4u}}}, u32);
    const auto structure = module.Type(spv::OpTypeStruct, array);
    module.AddAnnotation(spv::OpDecorate, structure, spv::DecorationBlock);
    module.AddAnnotation(spv::OpMemberDecorate, structure, 0u, spv::DecorationOffset, 0u);
    const auto buffer = module.DefineGlobalVariable(module.Type(spv::OpTypePointer, spv::StorageClassStorageBuffer, structure), spv::StorageClassStorageBuffer);
    module.AddAnnotation(spv::OpDecorate, buffer, spv::DecorationDescriptorSet, 0u);
    module.AddAnnotation(spv::OpDecorate, buffer, spv::DecorationBinding, 0u);
    const auto element = module.Type(spv::OpTypePointer, spv::StorageClassStorageBuffer, u32);
    const auto main = module.AllocateId();
    const auto voidType = module.Type(spv::OpTypeVoid);
    module.EmitEntryPoint(spv::ExecutionModelGLCompute, main, "main", {});
    module.AddExecutionMode(main, spv::ExecutionModeLocalSize, 1u, 1u, 1u);
    for (const auto width : {32u, 64u}) module.AddExecutionMode(main, spv::ExecutionModeSignedZeroInfNanPreserve, width);
    module.AddFunction(spv::OpFunction, voidType, main, spv::FunctionControlMaskNone, module.Type(spv::OpTypeFunction, voidType));
    module.AddFunction(spv::OpLabel, module.AllocateId());
    const auto address = [&](std::uint32_t index) {
        const auto pointer = module.AllocateId();
        module.AddFunction(spv::OpAccessChain, element, pointer, buffer, ConstantU32(state, 0u), ConstantU32(state, index));
        return pointer;
    };
    const auto load = [&](std::uint32_t index) {
        const auto word = module.AllocateId();
        module.AddFunction(spv::OpLoad, u32, word, address(index));
        return Unary(state, spv::OpBitcast, TypeF32(state), word);
    };
    for (std::uint32_t row = 0; row < Vectors.size(); ++row) {
        const auto delta = load(row * 4u);
        const auto coordinate = load(row * 4u + 1u);
        const auto base = load(row * 4u + 2u);
        const auto value = row < 3u ? EmitFPInterpolateF32(state, delta, coordinate, base) : EmitFPInterpolateF16(state, delta, coordinate, base);
        const auto result = row < 3u ? Unary(state, spv::OpBitcast, u32, value) : EmitConvertF16F32(state, value);
        module.AddFunction(spv::OpStore, address(row * 4u + 3u), result);
    }
    module.AddFunction(spv::OpReturn);
    module.AddFunction(spv::OpFunctionEnd);
    RecompileResult result{};
    result.spirv = module.Finalize();
    const auto base = reinterpret_cast<std::uintptr_t>(Data.data());
    result.bindings.push_back({DescriptorKind::StorageBuffer, DescriptorRole::GuestBuffers, 0u, 0u, 1u,
        {static_cast<std::uint32_t>(base), static_cast<std::uint32_t>((base >> 32u) & 0xffffu), static_cast<std::uint32_t>(sizeof(Data)), 0x01016facu}, false});
    return result;
}

void Run(AgcDriver::VulkanDevice& device, const RecompileResult& shader, std::uint32_t permutation) {
    Data.fill(Sentinel);
    std::array<std::uint32_t, Vectors.size()> indices{};
    for (std::uint32_t row = 0; row < Vectors.size(); ++row) {
        indices[row] = (row / 3u) * 3u + (row + permutation) % 3u;
        std::copy(Vectors[indices[row]].input.begin(), Vectors[indices[row]].input.end(), Data.begin() + row * 4u);
    }
    device.Dispatch(shader, 1u, 1u, 1u);
    device.WaitIdle();
    for (std::uint32_t row = 0; row < Vectors.size(); ++row) {
        const auto& vector = Vectors[indices[row]];
        for (std::uint32_t input = 0; input < vector.input.size(); ++input) Require(Data[row * 4u + input] == vector.input[input], "interpolation arithmetic changed an input");
        char message[160];
        std::snprintf(message, sizeof(message), "interpolation arithmetic permutation %u, native row %u: got 0x%08x, expected 0x%08x", permutation, indices[row], Data[row * 4u + 3u], vector.expected);
        Require(Data[row * 4u + 3u] == vector.expected, message);
    }
    for (std::size_t index = Vectors.size() * 4u; index < Data.size(); ++index) Require(Data[index] == Sentinel, "interpolation arithmetic wrote beyond the result rows");
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const auto target = device->Target();
        if (std::find(target.supportedCapabilities.begin(), target.supportedCapabilities.end(), spv::CapabilityFloat64) == target.supportedCapabilities.end()) {
            std::puts("skipped, interpolation arithmetic requires Float64");
            return VulkanTestSkipped;
        }
        const auto shader = Compile();
        for (const auto permutation : {0u, 1u}) Run(*device, shader, permutation);
        std::puts("interpolation arithmetic passed: 6 published native rows, 2 dispatches, 12 output comparisons");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
