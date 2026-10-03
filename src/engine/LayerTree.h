#pragma once
// Structure of the layer list: order and nesting, by layer id, independent of the layers themselves
// (undo of moves and grouping, drag and drop in the list, normalization after any change).
//
// Three levels: a viewport holds layers and groups, a group holds layers.
// A viewport is what one screen or projector shows; everything is rendered inside one.
//
// Invariants (restored by normalized()):
//  - the children of a viewport or a group immediately follow it, in their relative order;
//  - viewports are the only items at the root, and there is at least one as soon as anything exists;
//  - a group lives in a viewport, never in another group;
//  - a layer lives in a group or directly in a viewport;
//  - anything whose parent is gone falls back into the first viewport.

#include <QtGlobal>
#include <algorithm>
#include <map>
#include <set>
#include <vector>

struct TreeNode {
    enum Kind { Item = 0, Group = 1, Viewport = 2 };
    quint64 id = 0, parent = 0;
    Kind kind = Item;
    bool isGroup = false; // group or viewport: holds other items
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

inline quint64 firstViewport(const LayerTree &t)
{
    for (const TreeNode &n : t)
        if (n.kind == TreeNode::Viewport) return n.id;
    return 0;
}

inline LayerTree normalized(const LayerTree &t)
{
    std::set<quint64> viewports, groups;
    for (const TreeNode &n : t) {
        if (n.kind == TreeNode::Viewport) viewports.insert(n.id);
        else if (n.kind == TreeNode::Group) groups.insert(n.id);
    }
    const quint64 fallback = firstViewport(t);
    LayerTree roots;
    std::map<quint64, LayerTree> children;
    for (TreeNode n : t) {
        n.isGroup = n.kind != TreeNode::Item;
        if (n.kind == TreeNode::Viewport) {
            n.parent = 0;             // a viewport is never inside anything
        } else if (n.kind == TreeNode::Group) {
            if (!viewports.count(n.parent)) n.parent = fallback; // a group lives in a viewport
        } else if (!viewports.count(n.parent) && !groups.count(n.parent)) {
            n.parent = fallback;
        }
        if (n.kind == TreeNode::Viewport) roots.push_back(n);
        else children[n.parent].push_back(n);
    }
    LayerTree out;
    out.reserve(t.size());
    for (const TreeNode &v : roots) {
        out.push_back(v);
        for (const TreeNode &c : children[v.id]) {
            out.push_back(c);
            if (c.kind == TreeNode::Group)
                for (const TreeNode &m : children[c.id]) out.push_back(m);
        }
    }
    // Nothing is lost if there is no viewport at all (an empty project, or one being built)
    if (roots.empty())
        for (const TreeNode &n : t) {
            TreeNode c = n;
            c.parent = 0;
            c.isGroup = c.kind != TreeNode::Item;
            out.push_back(c);
        }
    return out;
}

// Ids of the children of a viewport or a group, in order
inline std::vector<quint64> members(const LayerTree &t, quint64 parent)
{
    std::vector<quint64> out;
    if (!parent) return out;
    for (const TreeNode &n : t)
        if (n.parent == parent) out.push_back(n.id);
    return out;
}

// Every item rendered inside a viewport, groups and their layers alike
inline std::vector<quint64> descendants(const LayerTree &t, quint64 parent)
{
    std::vector<quint64> out;
    for (quint64 id : members(t, parent)) {
        out.push_back(id);
        for (quint64 m : members(t, id)) out.push_back(m);
    }
    return out;
}

// Viewport an item is rendered in (itself if it is one)
inline quint64 viewportOf(const LayerTree &t, quint64 id)
{
    const int i = indexOf(t, id);
    if (i < 0) return 0;
    const TreeNode &n = t[size_t(i)];
    if (n.kind == TreeNode::Viewport) return n.id;
    if (kindOf(t, n.parent) == TreeNode::Viewport) return n.parent;
    return viewportOf(t, n.parent);
}

// Moves `ids` (a group or a viewport brings its contents along) before `beforeId` (0: at the end),
// into `parent` (0: the root, which only accepts viewports).
inline LayerTree moved(const LayerTree &t, const std::vector<quint64> &ids, quint64 beforeId, quint64 parent)
{
    std::set<quint64> moving(ids.begin(), ids.end());
    for (const TreeNode &n : t)
        if (n.kind != TreeNode::Item && moving.count(n.id))
            for (quint64 m : descendants(t, n.id)) moving.insert(m);
    if (moving.count(parent)) parent = 0; // never into itself
    LayerTree chunk, rest;
    for (TreeNode n : t) {
        if (!moving.count(n.id)) {
            rest.push_back(n);
            continue;
        }
        if (n.kind == TreeNode::Viewport) n.parent = 0;       // stays at the root
        else if (!(n.parent && moving.count(n.parent)))       // contents of something moving stay inside it
            n.parent = parent;
        chunk.push_back(n);
    }
    int at = beforeId ? indexOf(rest, beforeId) : -1;
    if (at < 0) at = int(rest.size());
    rest.insert(rest.begin() + at, chunk.begin(), chunk.end());
    return normalized(rest);
}

// Moves `ids` to the end of the contents of `parent` (a group or a viewport)
inline LayerTree intoGroup(const LayerTree &t, const std::vector<quint64> &ids, quint64 parent)
{
    const TreeNode::Kind pk = kindOf(t, parent);
    std::vector<quint64> mv;
    for (quint64 id : ids) {
        const TreeNode::Kind k = kindOf(t, id);
        if (indexOf(t, id) < 0 || id == parent) continue;
        if (k == TreeNode::Viewport) continue;                       // a viewport stays at the root
        if (k == TreeNode::Group && pk != TreeNode::Viewport) continue; // no group inside a group
        mv.push_back(id);
    }
    const int g = indexOf(t, parent);
    if (g < 0 || mv.empty()) return t;
    // Insert before the first item after the parent's block
    quint64 before = 0;
    const std::set<quint64> inside = [&] {
        std::set<quint64> s;
        for (quint64 id : descendants(t, parent)) s.insert(id);
        return s;
    }();
    for (size_t k = size_t(g) + 1; k < t.size(); ++k) {
        if (inside.count(t[k].id) || std::find(mv.begin(), mv.end(), t[k].id) != mv.end()) continue;
        before = t[k].id;
        break;
    }
    return moved(t, mv, before, parent);
}

// The group's layers go back into its viewport, in place of the group (which stays, empty, after them).
inline LayerTree ungrouped(const LayerTree &t, quint64 group)
{
    const quint64 vp = viewportOf(t, group);
    LayerTree out, mem;
    for (const TreeNode &n : t)
        if (n.parent == group) mem.push_back(n);
    for (const TreeNode &n : t) {
        if (n.parent == group) continue;
        if (n.id == group) {
            for (TreeNode m : mem) {
                m.parent = vp;
                out.push_back(m);
            }
        }
        out.push_back(n);
    }
    return normalized(out);
}

} // namespace tree
