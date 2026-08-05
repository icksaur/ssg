#include <ssg/ChromeLowering.h>

#include <ssg/GraphemeLayout.h>
#include <ssg/Theme.h>  // semanticRoleFromName
#include <ssg/Widget.h>

#include <string>
#include <utility>

namespace ssg {

namespace {

// One packed widget: its synthetic stack id (so placement lookup never assumes
// author id uniqueness), its source descriptor, and the resolved projection.
struct Packed {
    std::string stackId;
    const WidgetDescriptor* descriptor;
    std::string content;
    std::string label;
    std::optional<std::string> command;
    SemanticRole role;
};

struct Resolved {
    std::string content;
    std::string label;
    std::optional<std::string> command;
    bool drop = false;
};

bool truthy(std::string_view value) { return value == "true"; }

// Resolve a widget's displayed content + accessible label + inherited command.
Resolved resolveWidget(const WidgetDescriptor& w, const Style& style,
                       const ChromeProviderResolver& resolveProvider) {
    Resolved r;
    std::string value;
    std::string providerLabel;
    std::optional<std::string> inheritedCommand;
    bool fromProvider = false;
    if (w.value) {
        if (w.value->isProvider) {
            fromProvider = true;
            if (const auto resolved = resolveProvider(w.value->provider)) {
                value = resolved->value;
                providerLabel = resolved->accessibleLabel;
                inheritedCommand = resolved->commandId;
            }
        } else {
            value = w.value->literal;
        }
    }

    switch (w.kind) {
    case WidgetKind::Label:
    case WidgetKind::Field: {
        // A provider widget's label is the provider's; a literal widget labels
        // itself with its own text. Match the built-in status-field skip: drop
        // when EITHER the value or the accessible label is empty.
        const std::string label = fromProvider ? providerLabel : value;
        if (value.empty() || label.empty()) {
            r.drop = true;
            return r;
        }
        r.content = value;
        r.label = label;
        break;
    }
    case WidgetKind::Checkbox: {
        bool checked = false;
        if (w.checked) {
            if (w.checked->isProvider) {
                const auto resolved = resolveProvider(w.checked->provider);
                checked = resolved && truthy(resolved->value);
            } else {
                checked = truthy(w.checked->literal);
            }
        }
        r.content = checkboxText(checked, value, style.toggle);
        r.label = providerLabel.empty() ? r.content : providerLabel;
        break;
    }
    case WidgetKind::Spacer:
        r.content = {};  // a blank gap
        r.label = {};
        break;
    default:
        break;  // TextInput/Container are excluded by the decoder
    }

    // The descriptor's own command overrides an inherited one.
    r.command = w.command ? w.command : inheritedCommand;
    return r;
}

int widgetDesired(const WidgetDescriptor& w, const Resolved& resolved) {
    if (w.kind == WidgetKind::Spacer) return w.width.value_or(0);
    return measureFieldCells(resolved.content);
}

SemanticRole widgetRole(const WidgetDescriptor& w, SemanticRole defaultRole) {
    if (w.role) {
        if (const auto parsed = semanticRoleFromName(*w.role)) return *parsed;
    }
    return defaultRole;
}

StackItem stackItemFor(const WidgetDescriptor& w, std::string stackId,
                       const Resolved& resolved) {
    StackItem item;
    item.id = std::move(stackId);
    item.content = resolved.content;
    item.desired = widgetDesired(w, resolved);
    item.rank = w.rank;
    item.keep = w.keep;
    item.overflow = w.overflow;
    item.sigil = w.sigil;
    return item;
}

}  // namespace

void lowerChromeRow(const RowDescriptor& row, const Rect& rect,
                    ShellNodeKind nodeKind, SemanticRole defaultRole,
                    const Style& style,
                    const ChromeProviderResolver& resolveProvider,
                    std::vector<AccessibilityNode>& out) {
    WidgetStack stack{row.separator};
    std::vector<Packed> packed;

    const auto pack = [&](const WidgetDescriptor& w, const std::string& stackId,
                          bool isLeft, bool isCenter) {
        const Resolved resolved = resolveWidget(w, style, resolveProvider);
        if (resolved.drop) return;
        StackItem item = stackItemFor(w, stackId, resolved);
        if (isCenter) {
            stack.center(std::move(item), row.centerWidth, row.centerFixed);
        } else if (isLeft) {
            stack.packLeft(std::move(item));
        } else {
            stack.packRight(std::move(item));
        }
        packed.push_back({stackId, &w, resolved.content, resolved.label,
                          resolved.command, widgetRole(w, defaultRole)});
    };

    for (std::size_t i = 0; i < row.left.size(); ++i)
        pack(row.left[i], "L" + std::to_string(i), true, false);
    for (std::size_t i = 0; i < row.right.size(); ++i)
        pack(row.right[i], "R" + std::to_string(i), false, false);
    if (row.center) pack(*row.center, "C", false, true);

    const auto solved = stack.resolve(rect.width);
    if (!solved) return;  // a well-formed row cannot fail; guard defensively

    const auto emit = [&](std::string_view stackId) {
        const StackPlacement* placement = nullptr;
        for (const auto& p : solved->placed)
            if (p.id == stackId) placement = &p;
        if (!placement) return;
        const Packed* item = nullptr;
        for (const auto& p : packed)
            if (p.stackId == stackId) item = &p;
        if (!item) return;
        // A Spacer occupies stack space but emits no node -- it is a blank gap,
        // not an interactive element.
        if (item->descriptor->kind == WidgetKind::Spacer) return;
        out.push_back({nodeKind, item->descriptor->id, item->label,
                       {rect.x + placement->offset, rect.y, placement->size, 1},
                       item->role, item->content, item->command});
    };

    // Emit left → center → right, so hit-test order is deterministic.
    for (std::size_t i = 0; i < row.left.size(); ++i)
        emit("L" + std::to_string(i));
    if (row.center) emit("C");
    for (std::size_t i = 0; i < row.right.size(); ++i)
        emit("R" + std::to_string(i));
}

}  // namespace ssg
