#include "DX12ResourceStateTracker.h"
#include "../../Core/FFLog.h"

namespace RHI {
namespace DX12 {

// ============================================
// CDX12ResourceStateTracker Implementation
// ============================================

void CDX12ResourceStateTracker::RegisterResource(ID3D12Resource* resource, D3D12_RESOURCE_STATES initialState) {
    if (!resource) return;
    m_resourceStates[resource] = initialState;
}

void CDX12ResourceStateTracker::UnregisterResource(ID3D12Resource* resource) {
    if (!resource) return;
    m_resourceStates.erase(resource);
}

bool CDX12ResourceStateTracker::TransitionResource(
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES targetState,
    UINT subresource
) {
    if (!resource) return false;

    // Get current state
    auto it = m_resourceStates.find(resource);
    D3D12_RESOURCE_STATES currentState;

    if (it != m_resourceStates.end()) {
        currentState = it->second;
    } else {
        // Unknown resource - assume common state and register it
        currentState = D3D12_RESOURCE_STATE_COMMON;
        m_resourceStates[resource] = currentState;
        CFFLog::Warning("[ResourceStateTracker] Resource not registered, assuming COMMON state");
    }

    return TransitionResourceExplicit(resource, currentState, targetState, subresource);
}

bool CDX12ResourceStateTracker::TransitionResourceExplicit(
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES currentState,
    D3D12_RESOURCE_STATES targetState,
    UINT subresource
) {
    if (!resource) return false;

    // Check if transition is needed
    if (!NeedsTransition(currentState, targetState)) {
        return false;
    }

    // Create barrier
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = currentState;
    barrier.Transition.StateAfter = targetState;
    barrier.Transition.Subresource = subresource;

    m_pendingBarriers.push_back(barrier);

    // Update tracked state
    m_resourceStates[resource] = targetState;

    return true;
}

void CDX12ResourceStateTracker::UAVBarrier(ID3D12Resource* resource) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.UAV.pResource = resource;  // Can be nullptr for all UAVs

    m_pendingBarriers.push_back(barrier);
}

void CDX12ResourceStateTracker::AliasingBarrier(ID3D12Resource* resourceBefore, ID3D12Resource* resourceAfter) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Aliasing.pResourceBefore = resourceBefore;
    barrier.Aliasing.pResourceAfter = resourceAfter;

    m_pendingBarriers.push_back(barrier);
}

bool CDX12ResourceStateTracker::FlushBarriers(ID3D12GraphicsCommandList* cmdList) {
    if (m_pendingBarriers.empty()) {
        return false;
    }

    cmdList->ResourceBarrier(
        static_cast<UINT>(m_pendingBarriers.size()),
        m_pendingBarriers.data()
    );

    m_pendingBarriers.clear();
    return true;
}

D3D12_RESOURCE_STATES CDX12ResourceStateTracker::GetResourceState(ID3D12Resource* resource) const {
    auto it = m_resourceStates.find(resource);
    if (it != m_resourceStates.end()) {
        return it->second;
    }
    return D3D12_RESOURCE_STATE_COMMON;
}

bool CDX12ResourceStateTracker::IsResourceTracked(ID3D12Resource* resource) const {
    return m_resourceStates.find(resource) != m_resourceStates.end();
}

void CDX12ResourceStateTracker::Reset() {
    m_resourceStates.clear();
    m_pendingBarriers.clear();
}

// ============================================
// CDX12GlobalResourceStateManager Implementation
// ============================================

CDX12GlobalResourceStateManager& CDX12GlobalResourceStateManager::Instance() {
    static CDX12GlobalResourceStateManager instance;
    return instance;
}

void CDX12GlobalResourceStateManager::RegisterResource(ID3D12Resource* resource, D3D12_RESOURCE_STATES initialState) {
    if (!resource) return;
    m_states[resource] = initialState;
}

void CDX12GlobalResourceStateManager::UnregisterResource(ID3D12Resource* resource) {
    if (!resource) return;
    m_states.erase(resource);
}

D3D12_RESOURCE_STATES CDX12GlobalResourceStateManager::GetState(ID3D12Resource* resource) const {
    auto it = m_states.find(resource);
    if (it != m_states.end()) {
        return it->second;
    }
    return D3D12_RESOURCE_STATE_COMMON;
}

void CDX12GlobalResourceStateManager::SetState(ID3D12Resource* resource, D3D12_RESOURCE_STATES state) {
    if (!resource) return;
    m_states[resource] = state;
}

bool CDX12GlobalResourceStateManager::IsKnown(ID3D12Resource* resource) const {
    return m_states.find(resource) != m_states.end();
}

void CDX12GlobalResourceStateManager::Reset() {
    m_states.clear();
}

} // namespace DX12
} // namespace RHI
