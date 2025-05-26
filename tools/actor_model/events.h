#pragma once

#include <library/cpp/actors/core/event_local.h>

/**
 * Event definitions for Actor Model prime factorization system.
 */
struct TEvents {
    /// Event type enumeration for custom events
    enum EEventType : ui32 {
        EvWriteValueRequest = EventSpaceBegin(NActors::TEvents::ES_PRIVATE),
        EvTaskCompleted
    };

    /**
     * Event for sending computed largest prime factor to aggregation actor.
     */
    struct TEvWriteValueRequest
        : NActors::TEventLocal<TEvWriteValueRequest, EvWriteValueRequest> {

        const int64_t Value;

        explicit TEvWriteValueRequest(int64_t value) : Value(value) {}
    };

    /**
     * Event for signaling completion of prime factorization task.
     */
    struct TEvTaskCompleted
        : NActors::TEventLocal<TEvTaskCompleted, EvTaskCompleted> {
    };
};
