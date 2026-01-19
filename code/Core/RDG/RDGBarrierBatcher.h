#pragma once

#include "RDGTypes.h"
#include <d3d12.h>
#include <unordered_map>
#include <vector>

namespace RDG
{

//=============================================================================
// CRDGBarrierBatcher - Batches and flushes resource barriers
//=============================================================================

class CRDGBarrierBatcher
{
public:
    CRDGBarrierBatcher() = default;
    ~CRDGBarrierBatcher() = default;

    // Add a transition barrier
    void AddTransition(
        ID3D12Resource* resource,
        D3D12_RESOURCE_STATES stateBefore,
        D3D12_RESOURCE_STATES stateAfter,
        uint32_t subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);

    // Add an aliasing barrier (for placed resources sharing memory)
    void AddAliasing(
        ID3D12Resource* resourceBefore,  // Resource we're done using (can be nullptr)
        ID3D12Resource* resourceAfter);  // Resource we're about to use

    // Add a UAV barrier (for read-after-write hazards)
    void AddUAV(ID3D12Resource* resource);

    // Flush all pending barriers to command list
    void Flush(ID3D12GraphicsCommandList* cmdList);

    // Check if there are pending barriers
    bool HasPending() const { return !m_PendingBarriers.empty(); }

    // Clear without flushing (use with caution)
    void Clear() { m_PendingBarriers.clear(); }

private:
    std::vector<D3D12_RESOURCE_BARRIER> m_PendingBarriers;
};

//=============================================================================
// CRDGStateTracker - Tracks resource states across the frame
//=============================================================================

class CRDGStateTracker
{
public:
    CRDGStateTracker() = default;
    ~CRDGStateTracker() = default;

    // Set initial state for a resource
    void SetInitialState(ID3D12Resource* resource, D3D12_RESOURCE_STATES state);

    // Get current tracked state
    D3D12_RESOURCE_STATES GetCurrentState(ID3D12Resource* resource) const;

    // Record a state transition (updates internal tracking)
    void RecordTransition(ID3D12Resource* resource, D3D12_RESOURCE_STATES newState);

    // Check if resource is being tracked
    bool IsTracked(ID3D12Resource* resource) const;

    // Reset all tracking
    void Reset();

private:
    struct ResourceState
    {
        D3D12_RESOURCE_STATES CurrentState = D3D12_RESOURCE_STATE_COMMON;
    };

    std::unordered_map<ID3D12Resource*, ResourceState> m_States;
};

//=============================================================================
// Inline Implementations
//=============================================================================

inline void CRDGBarrierBatcher::AddTransition(
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES stateBefore,
    D3D12_RESOURCE_STATES stateAfter,
    uint32_t subresource)
{
    // Skip no-op transitions
    if (stateBefore == stateAfter || resource == nullptr)
        return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = stateBefore;
    barrier.Transition.StateAfter = stateAfter;
    barrier.Transition.Subresource = subresource;

    m_PendingBarriers.push_back(barrier);
}

inline void CRDGBarrierBatcher::AddAliasing(
    ID3D12Resource* resourceBefore,
    ID3D12Resource* resourceAfter)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Aliasing.pResourceBefore = resourceBefore;
    barrier.Aliasing.pResourceAfter = resourceAfter;

    m_PendingBarriers.push_back(barrier);
}

inline void CRDGBarrierBatcher::AddUAV(ID3D12Resource* resource)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.UAV.pResource = resource;

    m_PendingBarriers.push_back(barrier);
}

inline void CRDGBarrierBatcher::Flush(ID3D12GraphicsCommandList* cmdList)
{
    if (!m_PendingBarriers.empty())
    {
        cmdList->ResourceBarrier(
            static_cast<UINT>(m_PendingBarriers.size()),
            m_PendingBarriers.data());
        m_PendingBarriers.clear();
    }
}

inline void CRDGStateTracker::SetInitialState(ID3D12Resource* resource, D3D12_RESOURCE_STATES state)
{
    m_States[resource].CurrentState = state;
}

inline D3D12_RESOURCE_STATES CRDGStateTracker::GetCurrentState(ID3D12Resource* resource) const
{
    auto it = m_States.find(resource);
    if (it != m_States.end())
    {
        return it->second.CurrentState;
    }
    return D3D12_RESOURCE_STATE_COMMON;
}

inline void CRDGStateTracker::RecordTransition(ID3D12Resource* resource, D3D12_RESOURCE_STATES newState)
{
    m_States[resource].CurrentState = newState;
}

inline bool CRDGStateTracker::IsTracked(ID3D12Resource* resource) const
{
    return m_States.find(resource) != m_States.end();
}

inline void CRDGStateTracker::Reset()
{
    m_States.clear();
}

} // namespace RDG
