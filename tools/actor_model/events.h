#pragma once

#include <library/cpp/actors/core/event_local.h>

/**
 * Event definitions for Actor Model prime factorization system.
 * 
 * This system processes numbers from stdin, calculates their largest prime factors,
 * and outputs the sum of all largest prime factors.
 */
struct TEvents {
    /// Event type enumeration for custom events
    enum EEventType : ui32 {
        EvWriteValueRequest = EventSpaceBegin(NActors::TEvents::ES_PRIVATE),
        EvTaskCompleted
    };

    /**
     * Event for sending computed largest prime factor to aggregation actor.
     * Contains the largest prime factor of a processed number.
     */
    struct TEvWriteValueRequest 
        : NActors::TEventLocal<TEvWriteValueRequest, EvWriteValueRequest> {
        
        const int64_t Value;
        
        explicit TEvWriteValueRequest(int64_t value) : Value(value) {}
    };

    /**
     * Event for signaling completion of prime factorization task.
     * Sent from TMaximumPrimeDevisorActor to TReadActor when computation is finished.
     */
    struct TEvTaskCompleted 
        : NActors::TEventLocal<TEvTaskCompleted, EvTaskCompleted> {
    };
};
