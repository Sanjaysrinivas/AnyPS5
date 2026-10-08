#include "SceTypes.hpp"
#include "prx/libkernel/Pthread/include/Pthread.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <source_location>
#include <thread>

extern "C" {
int APS5_VABI sceKernelInstallExceptionHandler(int signum, void* handler);
int APS5_VABI sceKernelRemoveExceptionHandler(int signum);
int APS5_VABI sceKernelRaiseException(Pthread thread, int signum);
int APS5_VABI scePthreadCreate(Pthread* thread, const PthreadAttr* attr, PthreadEntry entry, void* arg, const char* name);
int APS5_VABI scePthreadJoin(Pthread thread, void** retval);
Pthread APS5_VABI scePthreadSelf();
int APS5_VABI sceKernelSyncOnAddressWait(std::uint32_t* address, std::uint32_t expected, const KernelUseconds* timeout, const char* name);
int APS5_VABI sceKernelSyncOnAddressWait8(std::uint8_t* address, std::uint8_t expected, const KernelUseconds* timeout, const char* name);
int APS5_VABI sceKernelSyncOnAddressWait16(std::uint16_t* address, std::uint16_t expected, const KernelUseconds* timeout, const char* name);
int APS5_VABI sceKernelSyncOnAddressWait32(std::uint32_t* address, std::uint32_t expected, const KernelUseconds* timeout, const char* name);
int APS5_VABI sceKernelSyncOnAddressWait64(std::uint64_t* address, std::uint64_t expected, const KernelUseconds* timeout, const char* name);
int APS5_VABI sceKernelSyncOnAddressWake(void* address, std::int32_t count);
}

static constexpr int RaisedSignal = 30;
static constexpr int NoThread = static_cast<int>(0x80020003);
static constexpr int InvalidArgument = static_cast<int>(0x80020016);
static constexpr int TimedOut = static_cast<int>(0x8002003c);
static constexpr KernelUseconds FailsafeTimeout = 60000000;
static constexpr KernelUseconds ShortTimeout = 10000;
static constexpr int ExitRounds = 64;

static void Require(bool value, std::source_location location = std::source_location::current()) {
    if (value) return;
    std::fprintf(stderr, "%s:%u: requirement failed\n", location.file_name(), location.line());
    std::abort();
}

template <class TPredicate>
static void Await(TPredicate predicate, std::source_location location = std::source_location::current()) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        Require(std::chrono::steady_clock::now() < deadline, location);
        std::this_thread::yield();
    }
}

struct DeliveryState {
    std::atomic<bool> started{false};
    std::atomic<bool> returned{false};
    std::atomic<bool> finish{false};
    std::atomic<bool> enteredHandler{false};
    std::atomic<bool> releaseHandler{false};
    std::atomic<int> delivered{0};
    std::atomic<Pthread> handlerThread{nullptr};
    std::atomic<std::uintptr_t> interruptedStack{0};
    std::atomic<std::uintptr_t> handlerStack{0};
    bool holdHandler = false;
};

struct RaiseState {
    std::atomic<bool> requested{false};
    std::atomic<bool> done{false};
    Pthread target = nullptr;
    int result = -1;
};

static void* APS5_VABI Raising(void* arg) {
    auto& state = *static_cast<RaiseState*>(arg);
    while (!state.requested.load()) {}
    state.result = sceKernelRaiseException(state.target, RaisedSignal);
    state.done.store(true);
    return nullptr;
}

static std::atomic<DeliveryState*> activeState{nullptr};
static_assert(std::atomic<DeliveryState*>::is_always_lock_free);
static_assert(std::atomic<Pthread>::is_always_lock_free);
static_assert(std::atomic<std::uintptr_t>::is_always_lock_free);
static_assert(std::atomic<int>::is_always_lock_free && std::atomic<bool>::is_always_lock_free);

static void APS5_VABI Handler(int signum, void* context) {
    Require(signum == RaisedSignal && context != nullptr);
    auto& state = *activeState.load();
    std::uintptr_t rsp = 0;
    std::memcpy(&rsp, static_cast<unsigned char*>(context) + 0xf8, sizeof(rsp));
    int local = 0;
    state.interruptedStack.store(rsp);
    state.handlerStack.store(reinterpret_cast<std::uintptr_t>(&local));
    state.handlerThread.store(scePthreadSelf());
    state.enteredHandler.store(true);
    while (state.holdHandler && !state.releaseHandler.load()) {}
    state.delivered.fetch_add(1);
}

static void VerifyDelivery(const DeliveryState& state, Pthread thread, int count) {
    Require(state.delivered.load() == count);
    Require(state.handlerThread.load() == thread);
    Require(state.interruptedStack.load() != 0);
    Require(state.handlerStack.load() < state.interruptedStack.load());
}

template <class TValue>
struct AddressState : DeliveryState {
    TValue word = 7;
    KernelUseconds timeout = FailsafeTimeout;
    int result = -1;
    std::chrono::steady_clock::duration elapsed{};
};

template <class TValue, auto Wait>
static void* APS5_VABI AddressWorker(void* arg) {
    auto& state = *static_cast<AddressState<TValue>*>(arg);
    const auto before = std::chrono::steady_clock::now();
    state.started.store(true);
    state.result = Wait(&state.word, TValue{7}, &state.timeout, "exception recovery");
    state.elapsed = std::chrono::steady_clock::now() - before;
    state.returned.store(true);
    while (!state.finish.load()) {}
    return &state.word;
}

template <class TValue, auto Wait>
static void AddressWaitRecovers(bool timeout, bool requestBeforeRelease) {
    AddressState<TValue> state;
    state.holdHandler = true;
    if (timeout) state.timeout = ShortTimeout;
    activeState.store(&state);
    TValue other = 7;
    std::atomic<bool> requestWake{false};
    std::atomic<bool> wakeStarted{false};
    std::atomic<bool> wakeDone{false};
    RaiseState raise;
    Pthread thread = nullptr;
    Pthread raiser = nullptr;
    Require(scePthreadCreate(&raiser, nullptr, Raising, &raise, "exception raiser") == 0);
    std::thread waker([&] {
        while (!requestWake.load()) {}
        if (!timeout) std::atomic_ref<TValue>(state.word).store(9);
        wakeStarted.store(true);
        Require(sceKernelSyncOnAddressWake(timeout ? &other : &state.word, 1) == 0);
        wakeDone.store(true);
    });
    Require(scePthreadCreate(&thread, nullptr, AddressWorker<TValue, Wait>, &state, "address recovery") == 0);
    Await([&] { return state.started.load(); });
    raise.target = thread;
    raise.requested.store(true);
    Await([&] { return state.enteredHandler.load(); });
    if (timeout || requestBeforeRelease) {
        requestWake.store(true);
        Await([&] { return wakeStarted.load(); });
    }
    state.releaseHandler.store(true);
    if (!timeout && !requestBeforeRelease) {
        Await([&] { return state.delivered.load() == 1; });
        requestWake.store(true);
    }
    Await([&] { return state.returned.load() && state.delivered.load() == 1; });
    Await([&] { return wakeDone.load(); });
    Await([&] { return raise.done.load(); });
    Require(scePthreadJoin(raiser, nullptr) == 0);
    waker.join();
    Require(raise.result == 0);
    VerifyDelivery(state, thread, 1);
    Require(state.result == (timeout ? TimedOut : 0));
    if (timeout) Require(state.elapsed >= std::chrono::microseconds(ShortTimeout));
    state.finish.store(true);
    void* result = nullptr;
    Require(scePthreadJoin(thread, &result) == 0 && result == &state.word);
    Require(sceKernelSyncOnAddressWake(&state.word, 1) == 0);
    std::atomic_ref<TValue>(state.word).store(7);
    const KernelUseconds noTime = 0;
    Require(Wait(&state.word, TValue{7}, &noTime, "reuse") == TimedOut);
    std::atomic_ref<TValue>(state.word).store(9);
    Require(Wait(&state.word, TValue{7}, &noTime, "reuse") == 0);
}

template <class TValue, auto Wait>
static void AddressWaits() {
    for (int round = 0; round < 4; ++round) {
        AddressWaitRecovers<TValue, Wait>(false, true);
        AddressWaitRecovers<TValue, Wait>(false, false);
        AddressWaitRecovers<TValue, Wait>(true, false);
    }
}

static void* APS5_VABI Exiting(void* arg) {
    auto& state = *static_cast<DeliveryState*>(arg);
    state.started.store(true);
    while (!state.finish.load()) {}
    return arg;
}

static void ExitRaces() {
    int accepted = 0;
    int rejected = 0;
    int delivered = 0;
    for (int round = 0; round < ExitRounds; ++round) {
        DeliveryState state;
        activeState.store(&state);
        Pthread thread = nullptr;
        Require(scePthreadCreate(&thread, nullptr, Exiting, &state, "exception exit") == 0);
        Await([&] { return state.started.load(); });
        Require(sceKernelRaiseException(thread, RaisedSignal) == 0);
        Await([&] { return state.delivered.load() == 1; });
        VerifyDelivery(state, thread, 1);
        ++accepted;
        state.finish.store(true);
        const int raced = sceKernelRaiseException(thread, RaisedSignal);
        Require(raced == 0 || raced == NoThread);
        accepted += raced == 0;
        rejected += raced == NoThread;
        Await([&] { return thread->_finished.load(std::memory_order_acquire); });
        Require(sceKernelRaiseException(thread, RaisedSignal) == NoThread);
        void* result = nullptr;
        Require(scePthreadJoin(thread, &result) == 0 && result == &state);
        const int calls = state.delivered.load();
        Require(calls >= 1 && calls <= 1 + (raced == 0));
        VerifyDelivery(state, thread, calls);
        delivered += calls;
    }
    std::printf("exit races: accepted=%d, delivered=%d, rejected=%d, finished rejections=%d\n",
                accepted, delivered, rejected, ExitRounds);
}

int main() {
    Require(sceKernelRaiseException(nullptr, RaisedSignal) == NoThread);
    for (int signal : {-1, 0, 1, 4, 8, 10, 11, 31, 128})
        Require(sceKernelRaiseException(nullptr, signal) == InvalidArgument);
    Require(sceKernelInstallExceptionHandler(RaisedSignal, reinterpret_cast<void*>(&Handler)) == 0);
    AddressWaits<std::uint8_t, sceKernelSyncOnAddressWait8>();
    AddressWaits<std::uint16_t, sceKernelSyncOnAddressWait16>();
    AddressWaits<std::uint32_t, sceKernelSyncOnAddressWait32>();
    AddressWaits<std::uint32_t, sceKernelSyncOnAddressWait>();
    AddressWaits<std::uint64_t, sceKernelSyncOnAddressWait64>();
    ExitRaces();
    Require(sceKernelRemoveExceptionHandler(RaisedSignal) == 0);
}
