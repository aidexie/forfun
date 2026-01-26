// Engine/Rendering/IPerFrameContributor.h
#pragma once

namespace RHI {
    class IDescriptorSet;
}

// Interface for subsystems that contribute bindings to the PerFrame descriptor set.
// Subsystems implement PopulatePerFrameSet() to bind their resources to Set 0.
class IPerFrameContributor {
public:
    virtual ~IPerFrameContributor() = default;

    // Called once per frame to populate PerFrame descriptor set bindings.
    // Subsystem should call set->Bind() with its resources using PerFrameSlots constants.
    virtual void PopulatePerFrameSet(RHI::IDescriptorSet* perFrameSet) = 0;
};
