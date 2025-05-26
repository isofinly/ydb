#include "actors.h"
#include <library/cpp/actors/core/executor_pool_basic.h>
#include <library/cpp/actors/core/scheduler_basic.h>
#include <util/generic/xrange.h>

namespace {
    constexpr ui32 DEFAULT_THREADS = 20;
    constexpr ui32 DEFAULT_POOLS = 1;
    constexpr ui32 EXECUTOR_POOL_SIZE = 512;
    constexpr ui32 SCHEDULER_POOL_SIZE = 512;
    constexpr auto MAIN_LOOP_SLEEP_INTERVAL = TDuration::MilliSeconds(200);
    constexpr auto SELF_PING_INTERVAL = TDuration::Seconds(1);
}

THolder<NActors::TActorSystemSetup> BuildActorSystemSetup(ui32 threads = DEFAULT_THREADS, ui32 pools = DEFAULT_POOLS) {
    auto setup = MakeHolder<NActors::TActorSystemSetup>();
    setup->ExecutorsCount = pools;
    setup->Executors.Reset(new TAutoPtr<NActors::IExecutorPool>[pools]);

    for (ui32 idx : xrange(pools)) {
        setup->Executors[idx] = new NActors::TBasicExecutorPool(idx, threads, EXECUTOR_POOL_SIZE);
    }

    setup->Scheduler.Reset(new NActors::TBasicSchedulerThread(
        NActors::TSchedulerConfig(SCHEDULER_POOL_SIZE, 0)));
    return setup;
}

int main(int argc, const char* argv[]) {
    Y_UNUSED(argc, argv);

    auto actorSystemSetup = BuildActorSystemSetup();
    NActors::TActorSystem actorSystem(actorSystemSetup);
    actorSystem.Start();

    actorSystem.Register(CreateSelfPingActor(SELF_PING_INTERVAL).Release());

    auto writeActorId = actorSystem.Register(CreateWriteActor().Release());
    actorSystem.Register(CreateReadActor(std::cin, writeActorId).Release());

    auto shouldContinue = GetProgramShouldContinue();
    while (shouldContinue->PollState() == TProgramShouldContinue::Continue) {
        Sleep(MAIN_LOOP_SLEEP_INTERVAL);
    }

    actorSystem.Stop();
    actorSystem.Cleanup();
    return shouldContinue->GetReturnCode();
}
