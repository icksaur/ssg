#pragma once

// A client's UI profile: the closed set of UI-VM primitives a given client
// IMPLEMENTATION can render. It exists to catch, early and loudly, a composition
// that uses a primitive the attached client cannot draw. It is a distinct type
// from CommandInvocation.h's CapabilityId (authorization: what a principal may
// do); a profile grants no permission and a capability declares no renderability.
//
// This is a passive value: it records which kinds are supported and answers
// queries. It does not select itself or police the wire. The promise that a
// profile is host-selected from the client build being served and never widened
// by a remote-client field belongs on the host/attach seam that will enforce it
// (a later phase), not on this type, which cannot keep it. A profile models only
// WidgetKind support in phase 2; the region-root role set and the
// mutation-operation set join it in later phases, when those vocabularies exist.

#include <ssg/Widget.h>

#include <array>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <stdexcept>

namespace ssg {

// The closed set of widget kinds a client renders. A value type: construct it
// with the kinds the client build implements, then query it. Default is the
// empty profile (renders nothing) so a client must positively declare support.
class ClientUiProfile {
public:
    ClientUiProfile() = default;
    ClientUiProfile(std::initializer_list<WidgetKind> supported) {
        for (const WidgetKind kind : supported) mark(kind);
    }

    // The profile that renders the whole current vocabulary. Named a factory, not
    // a default, so "supports everything" is always an explicit choice.
    [[nodiscard]] static ClientUiProfile full() {
        ClientUiProfile profile;
        profile.supported_.fill(true);
        return profile;
    }

    [[nodiscard]] bool supports(WidgetKind kind) const {
        return supported_[index(kind)];
    }

    // The rejection seam: the first widget kind in `kinds` this profile does not
    // support, or nullopt if it supports all of them (including an empty range).
    // The library composes against a profile and refuses a composition that
    // returns a value here, naming the unsupported kind via widgetKindName. A
    // range template so a caller may pass an initializer_list, a vector, or a walk
    // of a composition's kinds without materializing a container.
    template <typename Range>
    [[nodiscard]] std::optional<WidgetKind> firstUnsupported(
        Range const& kinds) const {
        for (const WidgetKind kind : kinds) {
            if (!supports(kind)) return kind;
        }
        return std::nullopt;
    }

    bool operator==(ClientUiProfile const&) const = default;

private:
    // A corrupt/out-of-range enumerator throws rather than indexing out of
    // bounds, so a profile cannot be constructed or queried with an invalid kind.
    static std::size_t index(WidgetKind kind) {
        const auto position = static_cast<std::size_t>(kind);
        if (position >= kWidgetKindCount) {
            throw std::invalid_argument("ClientUiProfile: unrecognized WidgetKind");
        }
        return position;
    }
    void mark(WidgetKind kind) { supported_[index(kind)] = true; }

    std::array<bool, kWidgetKindCount> supported_{};
};

}  // namespace ssg
