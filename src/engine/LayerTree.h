#pragma once
// Structure of the layer list: order and nesting, by layer id, independent of the layers themselves
// (undo of moves and grouping, drag and drop in the list, normalization after any change).
//
// Invariants (restored by normalized()):
//  - viewports come first, at the top level, in their relative order; they hold nothing;
//  - the contents of a group immediately follow it, in their relative order; a group can hold groups;
//  - a parent that is not an existing group (or would put a group inside itself) is cleared.

#include <QtGlobal>
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <vector>

struct TreeNode {
    enum Kind { Item = 0, Group = 1, Viewport = 2 };
    quint64 id = 0, parent = 0;
    Kind kind = Item;
    bool isGroup() const { return kind == Group; }
    bool operator==(const TreeNode &o) const { return id == o.id && parent == o.parent && kind == o.kind; }
    bool operator!=(const TreeNode &o) const { return !(*this == o); }
};
using LayerTree = std::vector<TreeNode>;

namespace tree {

inline int indexOf(const LayerTree &t, quint64 id)
{
    for (size_t i = 0; i < t.size(); ++i)
        if (t[i].id == id) return int(i);
    return -1;
}

inline TreeNode::Kind kindOf(const LayerTree &t, quint64 id)
{
    const int i = indexOf(t, id);
    return i < 0 ? TreeNode::Item : t[size_t(i)].kind;
}

inline LayerTree normalized(const LayerTree &t)
{
    std::map<quint64, quint64> parent;
    std::set<quint64> groups;
    for (const TreeNode &n : t) {
        if (n.kind == TreeNode::Group) groups.insert(n.id);
        parent[n.id] = n.parent;
    }
    // A parent must be a group; a chain of parents that comes back to the item is cut where it loops
    for (const TreeNode &n : t) {
        quint64 &p = parent[n.id];
        if (n.kind == TreeNode::Viewport || !groups.count(p)) {
            p = 0;
            continue;
        }
        std::set<quint64> seen{n.id};
        for (quint64 up = p; up; up = parent[up]) {
            if (seen.count(up)) {
                p = 0;
                break;
            }
            seen.insert(up);
        }
    }
    std::map<quint64, LayerTree> children;
    LayerTree viewports, top;
    for (TreeNode n : t) {
        n.parent = parent[n.id];
        if (n.kind == TreeNode::Viewport) viewports.push_back(n);
        else if (n.parent) children[n.parent].push_back(n);
        else top.push_back(n);
    }
    LayerTree out = viewports;
    out.reserve(t.size());
    std::function<void(const TreeNode &)> place = [&](const TreeNode &n) {
        out.push_back(n);
        if (n.kind == TreeNode::Group)
            for (const TreeNode &c : children[n.id]) place(c);
    };
    for (const TreeNode &n : top) place(n);
    return out;
}

// Ids of the direct contents of a group, in order
inline std::vector<quint64> members(const LayerTree &t, quint64 group)
{
    std::vector<quint64> out;
    if (!group) return out;
    for (const TreeNode &n : t)
        if (n.parent == group) out.push_back(n.id);
    return out;
}

// Everything inside a group, at any depth, in order
inline std::vector<quint64> descendants(const LayerTree &t, quint64 group)
{
    std::vector<quint64> out;
    for (quint64 id : members(t, group)) {
        out.push_back(id);
        for (quint64 d : descendants(t, id)) out.push_back(d);
    }
    return out;
}

// Moves `ids` (a group brings its contents along) before `beforeId` (0: at the end), into the group
// `parent` (0: top level). Viewports only move among viewports; nothing goes inside a viewport.
inline LayerTree moved(const LayerTree &t, const std::vector<quint64> &ids, quint64 beforeId, quint64 parent)
{
    if (kindOf(t, parent) != TreeNode::Group) parent = 0;
    std::set<quint64> moving(ids.begin(), ids.end());
    for (quint64 id : ids)
        for (quint64 d : descendants(t, id)) moving.insert(d);
    if (moving.count(parent)) parent = 0; // never into itself
    LayerTree chunk, rest;
    for (TreeNode n : t) {
        if (!moving.count(n.id)) {
            rest.push_back(n);
            continue;
        }
        if (n.kind == TreeNode::Viewport) n.parent = 0;
        else if (!(n.parent && moving.count(n.parent))) n.parent = parent; // contents of a moving group stay in it
        chunk.push_back(n);
    }
    int at = beforeId ? indexOf(rest, beforeId) : -1;
    if (at < 0) at = int(rest.size());
    rest.insert(rest.begin() + at, chunk.begin(), chunk.end());
    return normalized(rest);
}

// Moves `ids` to the end of the contents of `group`
inline LayerTree intoGroup(const LayerTree &t, const std::vector<quint64> &ids, quint64 group)
{
    const int g = indexOf(t, group);
    if (g < 0 || t[size_t(g)].kind != TreeNode::Group) return t;
    std::vector<quint64> mv;
    for (quint64 id : ids) {
        const int i = indexOf(t, id);
        if (i < 0 || id == group || t[size_t(i)].kind == TreeNode::Viewport) continue;
        const std::vector<quint64> inside = descendants(t, id);
        if (std::find(inside.begin(), inside.end(), group) != inside.end()) continue; // not into its own content
        mv.push_back(id);
    }
    if (mv.empty()) return t;
    // Insert before the first item after the group's block
    const std::vector<quint64> block = descendants(t, group);
    quint64 before = 0;
    for (size_t k = size_t(g) + 1; k < t.size(); ++k) {
        if (std::find(block.begin(), block.end(), t[k].id) != block.end()) continue;
        if (std::find(mv.begin(), mv.end(), t[k].id) != mv.end()) continue;
        before = t[k].id;
        break;
    }
    return moved(t, mv, before, group);
}

// The group's contents go up one level, in place of the group (which stays, empty, after them).
inline LayerTree ungrouped(const LayerTree &t, quint64 group)
{
    const int g = indexOf(t, group);
    const std::vector<quint64> inside = descendants(t, group);
    if (g < 0 || inside.empty()) return t;
    const quint64 up = t[size_t(g)].parent;
    LayerTree out;
    for (TreeNode n : t) {
        if (n.id == group) continue;
        if (n.parent == group) n.parent = up;
        out.push_back(n);
    }
    // The block keeps its place; the group, now empty, goes right after it
    out.insert(out.begin() + indexOf(out, inside.back()) + 1, t[size_t(g)]);
    return normalized(out);
}

// Depth of an item: 0 at the top level, 1 inside a group, 2 inside a group in a group…
inline int depthOf(const LayerTree &t, quint64 id)
{
    int d = 0;
    for (int i = indexOf(t, id); i >= 0 && t[size_t(i)].parent; i = indexOf(t, t[size_t(i)].parent)) ++d;
    return d;
}

} // namespace tree
