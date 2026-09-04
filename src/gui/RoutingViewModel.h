// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "document/Edit.h"

#include <optional>
#include <string>
#include <vector>

namespace dave::gui {

struct RoutingTargetOption {
    enum class Group { MainAndBuses, AudioTracks, HardwareOutputs };
    Group group = Group::MainAndBuses;
    std::string label;
    document::RouteTarget target;
    bool available = true;
    bool enabled = true;
    std::string disabledReason;
};

struct RoutingRequest {
    enum class Kind {
        SetHardwareInput,
        SetInputMonitor,
        SetMainOutput,
        AddSend,
        UpdateSend,
        RemoveSend,
        AddBus,
        RemoveBus,
        // Additional outputs beside the main route; `route` names which.
        AddOutput,
        RemoveOutput,
        // Make a fresh internal bus and point a new send / extra output at
        // it in one go — the "+ New bus…" entry in the target pickers.
        AddBusAndSend,
        AddBusAndOutput,
    };
    Kind kind = Kind::SetMainOutput;
    std::string ownerId;
    document::HardwareChannelSpan hardware;
    bool enabled = false;
    document::RouteTarget route = document::RouteTarget::none();
    document::AuxSend send;
};

class RoutingViewModel {
public:
    void request(RoutingRequest value) { pending_.push_back(std::move(value)); }
    std::vector<RoutingRequest> takeRequests() {
        auto result = std::move(pending_);
        pending_.clear();
        return result;
    }
    const std::vector<RoutingRequest>& requests() const { return pending_; }

private:
    std::vector<RoutingRequest> pending_;
};

std::vector<RoutingTargetOption> routingTargetOptions(
    const document::Edit& edit, const std::string& ownerId,
    int playbackChannels, bool forSend);

std::string routeTargetLabel(const document::Edit& edit,
                             const document::RouteTarget& target,
                             int playbackChannels);

} // namespace dave::gui
