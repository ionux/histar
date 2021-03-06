#ifndef BT_UTILS_H_
#define BT_UTILS_H_

#include <machine/types.h>
#include <kern/lib.h>
#include <inc/queue.h>
#include <btree/btree.h>

#define BTREE_IS_LEAF(node)	((node)->block.is_leaf)
#define BTREE_SET_LEAF(node)	do { (node)->block.is_leaf = 1; } while (0)

#define BTREE_NODE_SIZE(order, key_size) \
		(sizeof(struct btree_node) + \
		sizeof(offset_t) * order + \
		sizeof(uint64_t) * (order - 1) * (key_size))

#define BTREE_LEAF_ORDER(leaf) \
	(leaf->tree->order / leaf->tree->s_value)

struct btree_node;
struct btree;
TAILQ_HEAD(node_list, btree_node);

#define BTREE_OP_ATTR	static __inline __attribute__((always_inline))

BTREE_OP_ATTR const offset_t *
btree_key(struct btree_node *node, const int idx)
{
    int s_key = node->tree->s_key;
    return &node->keys[idx * s_key];
}

BTREE_OP_ATTR int64_t
btree_keycmp(const offset_t * key1, const offset_t * key2, int s_key)
{
    for (int i = 0; i < s_key; i++) {
	if (key1[i] > key2[i])
	    return 1;
	if (key1[i] < key2[i])
	    return -1;
    }

    return 0;
}

BTREE_OP_ATTR void
btree_keycpy(const offset_t * dst, const offset_t * src, int s_key)
{
    memcpy((offset_t *) dst, src, s_key * sizeof(offset_t));
}

BTREE_OP_ATTR void
btree_keymove(const offset_t * dst, const offset_t * src, int s_key)
{
    memmove((offset_t *) dst, src, s_key * sizeof(offset_t));
}

BTREE_OP_ATTR void
btree_keyset(const offset_t * dst, offset_t val, int s_key)
{
    offset_t *d = (offset_t *) dst;

    if (s_key) {
	do
	    *d++ = val;
	while (--s_key != 0);
    }
}

BTREE_OP_ATTR const offset_t *
btree_value(struct btree_node *node, const int idx)
{
    int s_val = node->tree->s_value;
    return &node->children[idx * s_val];
}

BTREE_OP_ATTR void
btree_valcpy(const offset_t * dst, const offset_t * src, int s_val)
{
    memcpy((offset_t *) dst, src, s_val * sizeof(offset_t));
}

BTREE_OP_ATTR void
btree_valmove(const offset_t * dst, const offset_t * src, int s_val)
{
    memmove((offset_t *) dst, src, s_val * sizeof(offset_t));
}

BTREE_OP_ATTR void
btree_valset(const offset_t * dst, offset_t val, int s_val)
{
    offset_t *d = (offset_t *) dst;

    if (s_val) {
	do
	    *d++ = val;
	while (--s_val != 0);
    }
}

BTREE_OP_ATTR void
bt_keycpy(struct btree_node *dst_node, int dst_idx,
	  struct btree_node *src_node, int src_idx)
{
    assert(dst_node->tree->s_key == src_node->tree->s_key);
    btree_keycpy(btree_key(dst_node, dst_idx),
		 btree_key(src_node, src_idx),
		 dst_node->tree->s_key);
}

BTREE_OP_ATTR void
bt_valcpy(struct btree_node *dst_node, int dst_idx,
	  struct btree_node *src_node, int src_idx)
{
    assert(dst_node->tree->s_value == src_node->tree->s_value);
    btree_valcpy(btree_value(dst_node, dst_idx),
		 btree_value(src_node, src_idx),
		 dst_node->tree->s_value);
}

#endif /*BT_UTILS_H_ */
