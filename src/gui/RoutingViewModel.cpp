// SPDX-License-Identifier: GPL-3.0-or-later
#include "gui/RoutingViewModel.h"

#include <algorithm>

namespace dave::gui {

namespace {

void appendOption(std::vector<RoutingTargetOption>& options,
                  const document::Edit& edit, const std::string& ownerId,
                  RoutingTargetOption::Group group, std::string label,
                  document::RouteTarget target, bool forSend,
                  bool available = true) {
    const auto validation = forSend
        ? edit.validateSendTarget(ownerId, target)
        : edit.validateMainOutput(ownerId, target);
    options.push_back({group, std::move(label), std::move(target), available,
                       validation.ok, validation.message});
}

} // namespace

std::vector<RoutingTargetOption> routingTargetOptions(
    const document::Edit& edit, const std::string& ownerId,
    int playbackChannels, bool forSend) {
    std::vector<RoutingTargetOption> options;
    const bool ownerIsMain = ownerId == document::kMainBusId;
    if (!ownerIsMain) {
        appendOption(options, edit, ownerId,
                     RoutingTargetOption::Group::MainAndBuses, "None",
                     document::RouteTarget::none(), forSend);
        for (const auto& bus : edit.tracks()) {
            appendOption(options, edit, ownerId,
                         RoutingTargetOption::Group::MainAndBuses, bus.name,
                         document::RouteTarget::bus(bus.id), forSend);
        }
        for (const auto& track : edit.tracks()) {
            appendOption(options, edit, ownerId,
                         RoutingTargetOption::Group::AudioTracks, track.name,
                         document::RouteTarget::audioTrack(track.id), forSend);
        }
    }

    const int channels = std::max(0, playbackChannels);
    for (int channel = 0; channel < channels; ++channel) {
        appendOption(options, edit, ownerId,
                     RoutingTargetOption::Group::HardwareOutputs,
                     "Output " + std::to_string(channel + 1),
                     document::RouteTarget::hardwareOutput(channel, 1), forSend);
    }
    for (int channel = 0; channel + 1 < channels; channel += 2) {
        appendOption(options, edit, ownerId,
                     RoutingTargetOption::Group::HardwareOutputs,
                     "Output " + std::to_string(channel + 1) + "-" +
                         std::to_string(channel + 2),
                     document::RouteTarget::hardwareOutput(channel, 2), forSend);
    }
    return options;
}

std::string routeTargetLabel(const document::Edit& edit,
                             const document::RouteTarget& target,
                             int playbackChannels) {
    switch (target.kind) {
        case document::RouteTarget::Kind::None:
            return "None";
        case document::RouteTarget::Kind::AudioTrack: {
            const auto* track = edit.track(target.targetId);
            return track ? track->name : "Missing audio track";
        }
        case document::RouteTarget::Kind::Bus: {
            const auto* bus = edit.track(target.targetId);
            return bus ? bus->name : "Missing bus";
        }
        case document::RouteTarget::Kind::HardwareOutput: {
            const int first = target.hardware.firstChannel;
            const int count = target.hardware.channelCount;
            std::string label = "Output " + std::to_string(first + 1);
            if (count == 2) label += "-" + std::to_string(first + 2);
            if (first < 0 || first + count > playbackChannels) {
                label += " (unavailable)";
            }
            return label;
        }
    }
    return "None";
}

} // namespace dave::gui
