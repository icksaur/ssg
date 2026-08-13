#pragma once

// The UI-VM tree schema: the medium-agnostic superset structure the library
// publishes once per generation. A schema is a set of region roots, each holding
// a generic node tree. A node is either a CONTAINER (an axis, an inset, and
// children, laid out by the medium-agnostic constraints in LayoutConstraints.h)
// or a LEAF (a WidgetDescriptor). Every node carries a UiNodeId unique within the
// schema's generation; patches, dynamic node state, triggers, and focus entries
// all address nodes by this id, so uniqueness is a precondition the validator
// enforces before any consumer walks the tree.
//
// The schema is immutable within a generation: a structural change is a full-tree
// replacement stamped with a new Generation. Values the tree DISPLAYS (resolved
// provider text, labels) and each node's present flag are NOT here -- they are
// generation-scoped dynamic state (a later sub-step). The schema carries value
// SOURCES (WidgetDescriptor's ValueSource), never resolved values, which is what
// keeps it immutable within a generation.

#include <ssg/ChromeComposition.h>   // WidgetDescriptor, ValueSource
#include <ssg/LayoutConstraints.h>   // Axis, Size, Inset
#include <ssg/RegionRoot.h>          // RegionRole

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ssg {

// Stamps a schema. A structural change bumps this; a mutation or dynamic-state
// delta names the generation it targets so a consumer on an old schema rejects
// rather than applying to the wrong tree.
struct Generation {
    explicit constexpr Generation(std::uint64_t value = 0) noexcept
        : value_{value} {}
    [[nodiscard]] constexpr std::uint64_t value() const noexcept {
        return value_;
    }
    constexpr auto operator<=>(Generation const&) const noexcept = default;

private:
    std::uint64_t value_;
};

// A node's identity, unique within a generation. A strong type over the authored
// id string so a raw widget/layout id (which is not guaranteed unique) can never
// be passed where a node identity is required.
class UiNodeId {
public:
    UiNodeId() = default;
    explicit UiNodeId(std::string value) : value_{std::move(value)} {}

    [[nodiscard]] std::string const& value() const noexcept { return value_; }
    [[nodiscard]] bool empty() const noexcept { return value_.empty(); }

    auto operator<=>(UiNodeId const&) const = default;

private:
    std::string value_;
};

struct UiNode;

// A container arranges its children on an axis, after reserving its inset.
struct UiContainer {
    Axis axis = Axis::Column;
    Inset inset;
    std::vector<UiNode> children;

    friend bool operator==(const UiContainer&, const UiContainer&) = default;
};

// A leaf renders one widget.
struct UiLeaf {
    WidgetDescriptor widget;

    friend bool operator==(const UiLeaf&, const UiLeaf&) = default;
};

// One node: an identity, a size within its parent, and either a container or a
// leaf. The variant makes "a node is exactly one of container/leaf" a type fact,
// not a pair of optionals that could both be set or both be empty.
struct UiNode {
    UiNodeId id;
    Size size;
    std::variant<UiContainer, UiLeaf> content;

    [[nodiscard]] bool isContainer() const noexcept {
        return std::holds_alternative<UiContainer>(content);
    }
    [[nodiscard]] bool isLeaf() const noexcept {
        return std::holds_alternative<UiLeaf>(content);
    }

    friend bool operator==(const UiNode&, const UiNode&) = default;
};

// A region root: a placement role and the node tree attached there.
struct UiRegion {
    RegionRole role;
    UiNode root;

    friend bool operator==(const UiRegion&, const UiRegion&) = default;
};

// A full schema for one generation: the region roots, each with its tree.
struct UiSchema {
    Generation generation{0};
    std::vector<UiRegion> regions;

    friend bool operator==(const UiSchema&, const UiSchema&) = default;
};

struct UiSchemaValidation {
    // A path-qualified message on failure (e.g. `top/root/header: duplicate node
    // id "header"`); nullopt on success.
    std::optional<std::string> error;

    [[nodiscard]] bool ok() const { return !error.has_value(); }
};

// Validate a schema before any consumer walks it. Enforces: every node id is
// non-empty and unique across the whole generation, and each region role appears
// at most once. Fails loud with a path-qualified message. Pure.
[[nodiscard]] UiSchemaValidation validateUiSchema(const UiSchema& schema);

// The set of every node id in a schema (all regions). Meaningful only for a
// schema whose ids are unique; used by ValidatedSchema.
[[nodiscard]] std::set<UiNodeId> uiSchemaNodeIds(const UiSchema& schema);

class ValidatedSchema;

struct ValidatedSchemaResult;

// A schema that has passed validateUiSchema. The only construction path is
// ValidatedSchema::validate, so a consumer that requires a ValidatedSchema (the
// mutation interpreter, the interaction state) cannot be handed a schema with
// duplicate or empty node ids -- the uniqueness the id-based rules depend on is a
// type fact at that boundary, not a runtime hope. It also caches the schema's node
// id set, so a capture or reference can be checked for schema membership.
class ValidatedSchema {
public:
    [[nodiscard]] static ValidatedSchemaResult validate(UiSchema schema);

    [[nodiscard]] const UiSchema& schema() const noexcept { return schema_; }
    [[nodiscard]] Generation generation() const noexcept {
        return schema_.generation;
    }
    [[nodiscard]] const std::set<UiNodeId>& nodeIds() const noexcept {
        return nodeIds_;
    }
    [[nodiscard]] bool contains(const UiNodeId& id) const {
        return nodeIds_.contains(id);
    }

private:
    explicit ValidatedSchema(UiSchema schema);

    UiSchema schema_;
    std::set<UiNodeId> nodeIds_;
};

// The outcome of validating a schema into a ValidatedSchema: exactly one of a
// validated schema or the path-qualified error, encoded as a variant so "both or
// neither" is unrepresentable. Defined after ValidatedSchema so the variant holds
// a complete type.
class ValidatedSchemaResult {
public:
    explicit ValidatedSchemaResult(ValidatedSchema schema)
        : value_{std::move(schema)} {}
    explicit ValidatedSchemaResult(std::string error)
        : value_{std::move(error)} {}

    [[nodiscard]] bool ok() const {
        return std::holds_alternative<ValidatedSchema>(value_);
    }
    [[nodiscard]] const ValidatedSchema& schema() const {
        return std::get<ValidatedSchema>(value_);
    }
    [[nodiscard]] ValidatedSchema takeSchema() {
        return std::get<ValidatedSchema>(std::move(value_));
    }
    [[nodiscard]] const std::string& error() const {
        return std::get<std::string>(value_);
    }

private:
    std::variant<ValidatedSchema, std::string> value_;
};

}  // namespace ssg
