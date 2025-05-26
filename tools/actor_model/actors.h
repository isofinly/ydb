#pragma once

#include <iostream>
#include <library/cpp/actors/core/actor.h>
#include <util/generic/ptr.h>
#include <library/cpp/actors/util/should_continue.h>

/**
 * Actor factory functions for prime factorization system.
 * 
 * The system consists of three main actor types:
 * - TReadActor: Reads numbers from input stream and spawns computation actors
 * - TMaximumPrimeDevisorActor: Computes largest prime factor for a single number
 * - TWriteActor: Aggregates results and outputs final sum
 */

/// Creates a self-monitoring actor that verifies system responsiveness
THolder<NActors::IActor> CreateSelfPingActor(const TDuration& latency);

/// Returns shared program continuation controller
std::shared_ptr<TProgramShouldContinue> GetProgramShouldContinue();

/// Creates input reading actor that manages the computation pipeline
THolder<NActors::IActor> CreateReadActor(std::istream& inputStream, 
                                          NActors::TActorId aggregatorActorId);

/// Creates output aggregation actor that collects and sums results
THolder<NActors::IActor> CreateWriteActor();

/// Creates computation actor for finding largest prime factor of a single number
THolder<NActors::IActor> CreateMaximumPrimeDevisorActor(int64_t number, 
                                                         NActors::TActorId originatorId,
                                                         NActors::TActorId collectorId);
