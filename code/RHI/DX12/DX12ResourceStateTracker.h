#pragma once

#include "DX12Common.h"
#include <unordered_map>
#include <vector>

// ============================================
// DX12 Resource State Tracker
// ============================================
// Tracks resource states and batches barrier submissions
// Simplifies automatic state transitions for upper layers

namespace RHI {
namespace DX12 {

// ============================================
// Pending Barrier
// ============================================
struct PendingBarrier {
    ID3D12Resource* resource;
    D3D12_RESOURCE_STATES stateBefore;
    D3D12_RESOURCE_STATES stateAfter;
    UINT subresource;
};

// ============================================
// Resource State Tracker
// ============================================
// Tracks the current state of resources and collects barriers
// Call FlushBarriers() before ExecuteCommandLists()

class CDX12ResourceStateTracker {
public:
    CDX12ResourceStateTracker() = default;
    ~CDX12ResourceStateTracker() = default;

    // Non-copyable
    CDX12ResourceStateTracker(const CDX12ResourceStateTracker&) = delete;
    CDX12ResourceStateTracker& operator=(const CDX12ResourceStateTracker&) = delete;

    // ============================================
    // State Tracking
    // ============================================

    // Register a resource with its initial state
    void RegisterResource(ID3D12Resource* resource, D3D12_RESOURCE_STATES initialState);

    // Unregister a resource (when it's destroyed)
    void UnregisterResource(ID3D12Resource* resource);

    // Request a state transition (will be batched)
    // Returns true if a barrier is needed
    bool TransitionResource(
        ID3D12Resource* resource,
        D3D12_RESOURCE_STATES targetState,
        UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES
    );

    // Request a state transition with explicit current state (preferred)
    // Use this when the caller knows the current state (e.g., from texture/buffer object)
    bool TransitionResourceExplicit(
        ID3D12Resource* resource,
        D3D12_RESOURCE_STATES currentState,
        D3D12_RESOURCE_STATES targetState,
        UINT subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES
    );

    // Request a UAV barrier (for same resource R/W sync)
    void UAVBarrier(ID3D12Resource* resource);

    // Request an aliasing barrier
    void AliasingBarrier(ID3D12Resource* resourceBefore, ID3D12Resource* resourceAfter);

    // ============================================
    // Barrier Submission
    // ============================================

    // Get pending barriers and clear the list
    // Returns true if there are barriers to submit
    bool FlushBarriers(ID3D12GraphicsCommandList* cmdList);

    // Check if there are pending barriers
    bool HasPendingBarriers() const { return !m_pendingBarriers.empty(); }

    // Get number of pending barriers
    size_t GetPendingBarrierCount() const { return m_pendingBarriers.size(); }

    // ============================================
    // Query State
    // ============================================

    // Get current tracked state of a resource
    D3D12_RESOURCE_STATES GetResourceState(ID3D12Resource* resource) const;

    // Check if resource is tracked
    bool IsResourceTracked(ID3D12Resource* resource) const;

    // Reset all tracking (e.g., at frame start)
    void Reset();

private:
    // Current known state of each resource
    std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> m_resourceStates;

    // Pending barriers to submit
    std::vector<D3D12_RESOURCE_BARRIER> m_pendingBarriers;
};

// ============================================
// Global Resource State Manager
// ============================================
// Singleton for managing global resource states across command lists
// In a multi-threaded scenario, each command list would have its own
// local tracker, and global state would be resolved at execution time.
// For our single-threaded case, this is simpler.

class CDX12GlobalResourceStateManager {
public:
    static CDX12GlobalResourceStateManager& Instance();

    // Register resource with initial state
    void RegisterResource(ID3D12Resource* resource, D3D12_RESOURCE_STATES initialState);

    // Unregister resource
    void UnregisterResource(ID3D12Resource* resource);

    // Get current state
    D3D12_RESOURCE_STATES GetState(ID3D12Resource* resource) const;

    // Update state (called after barriers are executed)
    void SetState(ID3D12Resource* resource, D3D12_RESOURCE_STATES state);

    // Check if resource is known
    bool IsKnown(ID3D12Resource* resource) const;

    // Clear all tracking
    void Reset();

private:
    CDX12GlobalResourceStateManager() = default;
    ~CDX12GlobalResourceStateManager() = default;

    // Non-copyable
    CDX12GlobalResourceStateManager(const CDX12GlobalResourceStateManager&) = delete;
    CDX12GlobalResourceStateManager& operator=(const CDX12GlobalResourceStateManager&) = delete;

private:
    std::unordered_map<ID3D12Resource*, D3D12_RESOURCE_STATES> m_states;
};

} // namespace DX12
} // namespace RHI
