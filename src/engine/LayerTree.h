#pragma once
// Structure of the layer list: order and grouping, by layer id, independent of the layers themselves
// (undo of moves and grouping, drag and drop in the list, normalization after any change).
//
// Invariants (restored by normalized()):
//  - the members of a group immediately follow it, in their relative order;
//  - groups are at the top level (no group inside a group);
//  - a parent that is not an existing group is cleared.

#include <QtGlobal>
#include <algorithm>
#include <map>
#include <set>
#include <vector>

struct TreeNode {
    quint64 id = 0, parent = 0;
    bool isGroup = false;
    bool operator==(const TreeNode &o) const { return id == o.id && parent == o.parent && isGroup == o.isGroup; }
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

inline LayerTree normalized(const LayerTree &t)
{
    std::set<quint64> groups;
    for (const TreeNode &n : t)
        if (n.isGroup) groups.insert(n.id);
    LayerTree top;
    std::map<quint64, LayerTree> members;
    for (TreeNode n : t) {
        if (n.isGroup || !groups.count(n.parent)) n.parent = 0;
        if (n.parent) members[n.parent].push_back(n);
        else top.push_back(n);
    }
    LayerTree out;
    out.reserve(t.size());
    for (const TreeNode &n : top) {
        out.push_back(n);
        if (n.isGroup)
            for (const TreeNode &m : members[n.id]) out.push_back(m);
    }
    return out;
}

// Ids of the members of a group, in order
inline std::vector<quint64> members(const LayerTree &t, quint64 group)
{
    std::vector<quint64> out;
    for (const TreeNode &n : t)
        if (n.parent == group && group) out.push_back(n.id);
    return out;
}

// Moves the items `ids` (a group brings its members along) before the item `beforeId` (0: at the end),
// into `parent` (0: top level). Groups always land at the top level.
inline LayerTree moved(const LayerTree &t, const std::vector<quint64> &ids, quint64 beforeId, quint64 parent)
{
    std::set<quint64> moving(ids.begin(), ids.end());
    for (const TreeNode &n : t)
        if (n.isGroup && moving.count(n.id))
            for (quint64 m : members(t, n.id)) moving.insert(m);
    if (moving.count(parent)) parent = 0; // never into itself
    LayerTree chunk, rest;
    for (TreeNode n : t) {
        if (!moving.count(n.id)) {
            rest.push_back(n);
            continue;
        }
        if (n.isGroup) n.parent = 0;
        else if (!(n.parent && moving.count(n.parent))) n.parent = parent; // members of a moving group stay in it
        chunk.push_back(n);
    }
    int at = beforeId ? indexOf(rest, beforeId) : -1;
    if (at < 0) at = int(rest.size());
    rest.insert(rest.begin() + at, chunk.begin(), chunk.end());
    return normalized(rest);
}

// Moves `ids` to the end of the members of `group`
inline LayerTree intoGroup(const LayerTree &t, const std::vector<quint64> &ids, quint64 group)
{
    std::vector<quint64> mv;
    for (quint64 id : ids) {
        const int i = indexOf(t, id);
        if (i >= 0 && !t[size_t(i)].isGroup && id != group) mv.push_back(id);
    }
    // Insert before the first item after the group's block
    const int g = indexOf(t, group);
    if (g < 0) return t;
    quint64 before = 0;
    for (size_t k = size_t(g) + 1; k < t.size(); ++k) {
        if (t[k].parent == group || std::find(mv.begin(), mv.end(), t[k].id) != mv.end()) continue;
        before = t[k].id;
        break;
    }
    return moved(t, mv, before, group);
}

// The group's members go back to the top level, in place of the group (which stays, empty, after them).
inline LayerTree ungrouped(const LayerTree &t, quint64 group)
{
    LayerTree out;
    LayerTree mem;
    for (const TreeNode &n : t)
        if (n.parent == group) mem.push_back(n);
    for (const TreeNode &n : t) {
        if (n.parent == group) continue;
        if (n.id == group) {
            for (TreeNode m : mem) {
                m.parent = 0;
                out.push_back(m);
            }
        }
        out.push_back(n);
    }
    return normalized(out);
}

} // namespace tree
