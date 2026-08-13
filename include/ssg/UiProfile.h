#pragma once

// A client's UI profile: the closed set of UI-VM primitives a given client
// IMPLEMENTATION can render. It exists to catch, early and loudly, a composition
// that uses a primitive the attached client cannot draw. It is a distinct type
// from CommandInvocation.h's CapabilityId (authorization: what a principal may
// do); a profile grants no permission and a capability declares no renderability.
//
// This is a passive value: it records which primitives are supported and answers
// queries. It does not select itself or police the wire. The promise that a
// profile is host-selected from the client build being served and never widened
// by a remote-client field belongs on the host/attach seam that will enforce it
// (a later phase), not on this type, which cannot keep it. A profile covers the
// widget vocabulary (WidgetKind) and the region-root placement vocabulary
// (RegionRole); the mutation-operation set joins it in a later phase, when that
// vocabulary exists.

#include <ssg/RegionRoot.h>
#include <ssg/Widget.h>

#include <array>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <stdexcept>

namespace ssg {

// The closed sets of widget kinds and region roles a client renders. A value
// type: declare the primitives the client build implements, then query it.
// Default is the empty profile (renders nothing) so a client must positively
// declare support.
class ClientUiProfile {
public:
    ClientUiProfile() = default;
    ClientUiProfile(std::initializer_list<WidgetKind> widgets) {
        for (const WidgetKind kind : widgets) allow(kind);
    }

    // The profile that renders the whole current vocabulary. Named a factory, not
    // a default, so "supports everything" is always an explicit choice.
    [[nodiscard]] static ClientUiProfile full() {
        ClientUiProfile profile;
        profile.widgets_.fill(true);
        profile.regions_.fill(true);
        return profile;
    }

    ClientUiProfile& allow(WidgetKind kind) {
        widgets_[index(kind)] = true;
        return *this;
    }
    ClientUiProfile& allow(RegionRole role) {
        regions_[index(role)] = true;
        return *this;
    }
    ClientUiProfile& allowRegions(std::initializer_list<RegionRole> roles) {
        for (const RegionRole role : roles) allow(role);
        return *this;
    }

    [[nodiscard]] bool supports(WidgetKind kind) const {
        return widgets_[index(kind)];
    }
    [[nodiscard]] bool supports(RegionRole role) const {
        return regions_[index(role)];
    }

    // The rejection seam: the first primitive in `items` this profile does not
    // support, or nullopt if it supports all of them (including an empty range).
    // The library composes against a profile and refuses a composition that
    // returns a value here, naming the unsupported primitive via its *Name()
    // helper. One template over both vocabularies, dispatched by the `supports`
    // overload for the range's element type, so a caller may pass a walk of a
    // composition's widget kinds or its region roles without a container.
    template <typename Range>
    [[nodiscard]] std::optional<std::ranges::range_value_t<Range>>
    firstUnsupported(Range const& items) const {
        for (auto const& item : items) {
            if (!supports(item)) return item;
        }
        return std::nullopt;
    }

    bool operator==(ClientUiProfile const&) const = default;

private:
    // A corrupt/out-of-range enumerator throws rather than indexing out of
    // bounds, so a profile cannot be constructed or queried with an invalid
    // primitive.
    static std::size_t index(WidgetKind kind) {
        const auto position = static_cast<std::size_t>(kind);
        if (position >= kWidgetKindCount) {
            throw std::invalid_argument("ClientUiProfile: unrecognized WidgetKind");
        }
        return position;
    }
    static std::size_t index(RegionRole role) {
        const auto position = static_cast<std::size_t>(role);
        if (position >= kRegionRoleCount) {
            throw std::invalid_argument("ClientUiProfile: unrecognized RegionRole");
        }
        return position;
    }

    std::array<bool, kWidgetKindCount> widgets_{};
    std::array<bool, kRegionRoleCount> regions_{};
};

}  // namespace ssg
