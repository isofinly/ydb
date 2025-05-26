#include "actors.h"
#include "events.h"

#include <library/cpp/actors/core/actor.h>
#include <library/cpp/actors/core/actor_bootstrapped.h>
#include <library/cpp/actors/core/hfunc.h>
#include <util/datetime/base.h>

namespace {
    /// Maximum computation time per actor cycle to avoid blocking
    constexpr auto MAX_COMPUTATION_TIME_SLICE = TDuration::MilliSeconds(10);

    /// Shared program state controller
    auto ShouldContinue = std::make_shared<TProgramShouldContinue>();
}

/**
 * Actor responsible for computing the largest prime factor of a single number.
 */
class TMaximumPrimeDevisorActor
    : public NActors::TActorBootstrapped<TMaximumPrimeDevisorActor> {

    const int64_t OriginalNumber;
    const NActors::TActorId OriginatorActorId;
    const NActors::TActorId ResultCollectorId;

    // Computation state
    int64_t RemainingValue;
    int64_t CurrentDivisor;
    int64_t LargestPrimeFactor;

public:
    TMaximumPrimeDevisorActor(int64_t number,
                              NActors::TActorId originator,
                              NActors::TActorId collector)
        : OriginalNumber(number)
        , OriginatorActorId(originator)
        , ResultCollectorId(collector)
        , CurrentDivisor(2) {

        // Initialize based on the original number
        if (OriginalNumber <= 0) {
            LargestPrimeFactor = 0;
            RemainingValue = 0;
        } else if (OriginalNumber == 1) {
            LargestPrimeFactor = 1;
            RemainingValue = 1;
        } else {
            LargestPrimeFactor = 1;
            RemainingValue = OriginalNumber;
        }
    }

    void Bootstrap() {
        Become(&TMaximumPrimeDevisorActor::StateFunc);
        ScheduleNextComputationCycle();
    }

    STRICT_STFUNC(StateFunc, {
        cFunc(NActors::TEvents::TEvWakeup::EventType, ProcessFactorization);
    });

private:
    void ProcessFactorization() {
        // Handle special cases immediately
        if (RemainingValue <= 1) {
            int64_t result = (RemainingValue == 0) ? 0 : LargestPrimeFactor;
            CompleteComputationAndExit(result);
            return;
        }

        const auto computationStartTime = TInstant::Now();

        // Handle factor of 2 separately for optimization
        if (CurrentDivisor == 2) {
            ExtractAllFactorsOf(2);
            CurrentDivisor = 3; // Move to odd numbers only

            if (IsComputationTimeLimitReached(computationStartTime)) {
                return;
            }
        }

        // Process odd divisors only (optimization)
        while (CurrentDivisor * CurrentDivisor <= RemainingValue) {
            if (IsComputationTimeLimitReached(computationStartTime)) {
                return;
            }

            ExtractAllFactorsOf(CurrentDivisor);
            CurrentDivisor += 2; // Skip even numbers
        }

        // If remaining value is > 1, it's a prime factor itself
        if (RemainingValue > 1) {
            LargestPrimeFactor = RemainingValue;
        }

        CompleteComputationAndExit(LargestPrimeFactor);
    }

    void ExtractAllFactorsOf(int64_t factor) {
        if (RemainingValue % factor == 0) {
            LargestPrimeFactor = factor;

            // Remove all instances of this factor
            while (RemainingValue % factor == 0) {
                RemainingValue /= factor;
            }
        }
    }

    bool IsComputationTimeLimitReached(TInstant startTime) {
        if (TInstant::Now() - startTime > MAX_COMPUTATION_TIME_SLICE) {
            ScheduleNextComputationCycle();
            return true;
        }
        return false;
    }

    void ScheduleNextComputationCycle() {
        Send(SelfId(), std::make_unique<NActors::TEvents::TEvWakeup>());
    }

    void CompleteComputationAndExit(int64_t result) {
        // Send result to aggregator
        Send(ResultCollectorId, MakeHolder<TEvents::TEvWriteValueRequest>(result));

        // Notify originator of completion
        Send(OriginatorActorId, MakeHolder<TEvents::TEvTaskCompleted>());

        // Terminate this actor
        PassAway();
    }
};

/**
 * Actor responsible for reading numbers from input stream and managing computation tasks.
 */
class TReadActor : public NActors::TActorBootstrapped<TReadActor> {
    std::istream& InputStream;
    const NActors::TActorId ResultAggregatorId;

    int32_t ActiveComputationTasks;
    bool InputStreamExhausted;

public:
    TReadActor(std::istream& inputStream, NActors::TActorId aggregatorId)
        : InputStream(inputStream)
        , ResultAggregatorId(aggregatorId)
        , ActiveComputationTasks(0)
        , InputStreamExhausted(false) {
    }

    void Bootstrap() {
        Become(&TReadActor::StateFunc);
        ScheduleNextInputRead();
    }

    STRICT_STFUNC(StateFunc, {
        cFunc(NActors::TEvents::TEvWakeup::EventType, ProcessNextInput);
        hFunc(TEvents::TEvTaskCompleted, HandleComputationTaskCompleted);
    });

private:
    void ProcessNextInput() {
        if (InputStreamExhausted) {
            TryCompleteProcessing();
            return;
        }

        int64_t inputValue;
        if (InputStream >> inputValue) {
            SpawnComputationTask(inputValue);
            ScheduleNextInputRead();
        } else {
            InputStreamExhausted = true;
            TryCompleteProcessing();
        }
    }

    void SpawnComputationTask(int64_t number) {
        Register(new TMaximumPrimeDevisorActor(number, SelfId(), ResultAggregatorId));
        ActiveComputationTasks++;
    }

    void HandleComputationTaskCompleted(TEvents::TEvTaskCompleted::TPtr&) {
        ActiveComputationTasks--;
        TryCompleteProcessing();
    }

    void TryCompleteProcessing() {
        if (InputStreamExhausted && ActiveComputationTasks == 0) {
            // Signal aggregator to output results and terminate
            Send(ResultAggregatorId, std::make_unique<NActors::TEvents::TEvPoisonPill>());
            PassAway();
        }
    }

    void ScheduleNextInputRead() {
        Send(SelfId(), std::make_unique<NActors::TEvents::TEvWakeup>());
    }
};

/**
 * Actor responsible for aggregating computation results and producing final output.
 */
class TWriteActor : public NActors::TActor<TWriteActor> {
    int64_t AccumulatedSum;

public:
    TWriteActor()
        : NActors::TActor<TWriteActor>(&TWriteActor::StateFunc)
        , AccumulatedSum(0) {
    }

    STRICT_STFUNC(StateFunc, {
        hFunc(TEvents::TEvWriteValueRequest, AccumulateResult);
        cFunc(NActors::TEvents::TEvPoisonPill::EventType, OutputResultsAndTerminate);
    });

private:
    void AccumulateResult(TEvents::TEvWriteValueRequest::TPtr& event) {
        AccumulatedSum += event->Get()->Value;
    }

    void OutputResultsAndTerminate() {
        Cout << AccumulatedSum << Endl;
        GetProgramShouldContinue()->ShouldStop(0);
        PassAway();
    }
};

/**
 * Monitoring actor that verifies system responsiveness.
 */
class TSelfPingActor : public NActors::TActorBootstrapped<TSelfPingActor> {
    const TDuration PingLatency;
    TInstant LastPingTime;

public:
    explicit TSelfPingActor(const TDuration& latency)
        : PingLatency(latency) {
    }

    void Bootstrap() {
        LastPingTime = TInstant::Now();
        Become(&TSelfPingActor::StateFunc);
        Send(SelfId(), std::make_unique<NActors::TEvents::TEvWakeup>());
    }

    void HandlePingResponse(NActors::TEvents::TEvWakeup::TPtr&) {
        const auto currentTime = TInstant::Now();
        const auto actualLatency = currentTime - LastPingTime;

        Y_VERIFY(actualLatency <= PingLatency, "System latency exceeded threshold");

        LastPingTime = currentTime;
        Send(SelfId(), std::make_unique<NActors::TEvents::TEvWakeup>());
    }

    STRICT_STFUNC(StateFunc, {
        hFunc(NActors::TEvents::TEvWakeup, HandlePingResponse);
    });
};

// Factory function implementations
THolder<NActors::IActor> CreateReadActor(std::istream& inputStream,
                                          NActors::TActorId aggregatorId) {
    return MakeHolder<TReadActor>(inputStream, aggregatorId);
}

THolder<NActors::IActor> CreateWriteActor() {
    return MakeHolder<TWriteActor>();
}

THolder<NActors::IActor> CreateSelfPingActor(const TDuration& latency) {
    return MakeHolder<TSelfPingActor>(latency);
}

THolder<NActors::IActor> CreateMaximumPrimeDevisorActor(int64_t number,
                                                         NActors::TActorId originator,
                                                         NActors::TActorId collector) {
    return MakeHolder<TMaximumPrimeDevisorActor>(number, originator, collector);
}

std::shared_ptr<TProgramShouldContinue> GetProgramShouldContinue() {
    return ShouldContinue;
}
