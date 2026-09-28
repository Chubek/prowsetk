#include "flatworm/dom_internal.hpp"

#include <algorithm>

namespace prowsetk::flatworm {
std::shared_ptr<Node> make_node(NodeType type) {
    auto node = std::make_shared<Node>();
    node->type = type;
    return node;
}

const std::string* Node::attribute(std::string_view attribute_name) const {
    for (const auto& attr : attributes) {
        if (attr.name == attribute_name) {
            return &attr.value;
        }
    }
    return nullptr;
}

void Node::set_attribute(std::string_view attribute_name,
                         std::string_view value) {
    for (auto& attr : attributes) {
        if (attr.name == attribute_name) {
            attr.value = std::string(value);
            report_mutation(MutationInfo{"attribute-set", name,
                                         std::string(attribute_name),
                                         std::string(value)});
            return;
        }
    }
    attributes.push_back(
        Attribute{std::string(attribute_name), std::string(value)});
    report_mutation(MutationInfo{"attribute-set", name,
                                 std::string(attribute_name),
                                 std::string(value)});
}

bool Node::remove_attribute(std::string_view attribute_name) {
    const auto it = std::remove_if(attributes.begin(), attributes.end(),
                                   [attribute_name](const Attribute& attr) {
                                       return attr.name == attribute_name;
                                   });
    if (it == attributes.end()) {
        return false;
    }
    attributes.erase(it, attributes.end());
    report_mutation(MutationInfo{"attribute-removed", name,
                                 std::string(attribute_name), {}});
    return true;
}

void Node::append_child(const std::shared_ptr<Node>& child) {
    if (child == nullptr || child.get() == this) {
        return;
    }
    const auto self = shared_from_this();
    for (auto ancestor = self; ancestor != nullptr; ancestor = ancestor->shared_parent()) {
        if (ancestor == child) {
            return;
        }
    }
    const auto old_parent = child->shared_parent();
    children.push_back(child);
    if (old_parent != nullptr) {
        const auto it = std::find(old_parent->children.begin(),
                                  old_parent->children.end(), child);
        if (it != old_parent->children.end()) {
            old_parent->children.erase(it);
        }
    }
    child->parent = self;
    if (old_parent != nullptr) {
        old_parent->report_mutation(MutationInfo{"child-removed", child->name, {}, {}});
    }
    report_mutation(MutationInfo{"child-added", child->name, {}, {}});
}

bool Node::remove_child(const std::shared_ptr<Node>& child) {
    if (child == nullptr) {
        return false;
    }
    const auto it = std::find(children.begin(), children.end(), child);
    if (it == children.end()) {
        return false;
    }
    const std::string removed_name = (*it)->name;
    (*it)->parent.reset();
    children.erase(it);
    report_mutation(MutationInfo{"child-removed", removed_name, {}, {}});
    return true;
}

std::shared_ptr<Node> Node::document_root() const {
    std::shared_ptr<Node> node =
        std::const_pointer_cast<Node>(shared_from_this());
    while (true) {
        std::shared_ptr<Node> ancestor = node->parent.lock();
        if (ancestor == nullptr) {
            return node;
        }
        node = ancestor;
    }
}

void Node::report_mutation(const MutationInfo& info) const {
    std::shared_ptr<Node> root;
    try {
        root = document_root();
    } catch (const std::bad_weak_ptr&) {
        return;
    }
    if (root != nullptr && root->mutation_sink != nullptr) {
        (*root->mutation_sink)(info);
    }
}


}  // namespace prowsetk::flatworm
