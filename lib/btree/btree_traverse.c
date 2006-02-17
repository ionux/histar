/**
 * @file btree_traverse.c Traversal functions
 * 
 * $Id: btree_traverse.c,v 1.9 2002/06/23 10:28:05 chipx86 Exp $
 *
 * @Copyright (C) 1999-2002 The GNUpdate Project.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA  02111-1307, USA.
 */
#include <lib/btree/btree_utils.h>
#include <lib/btree/btree_node.h>
#include <lib/btree/btree_header.h>
#include <lib/btree/btree_traverse.h>
#include <inc/stdio.h>
#include <inc/string.h>
#include <inc/error.h>

#if 0  // btree_traverse is broken
static struct btree_traversal *
btreeDestroyTraversal(struct btree_traversal *trav)
{
	if (trav == NULL)
		return NULL;

	if (trav->node != NULL)
		btree_destroy_node(trav->node);

	btree_release_nodes(trav->tree) ;

	return NULL;
}
#endif

#if 0  // broken
void
btree_traverse(struct btree *tree, void (*process)(offset_t filePos))
{
	struct btree_traversal trav;
	int64_t offset;
	
	if (tree == NULL || process == NULL)
		return;

	btree_init_traversal(tree, &trav);

	for (offset = btree_first_offset(&trav);
		 offset != -1;
		 offset = btree_next_entry(&trav))
	{
		process(offset);
	}

	btreeDestroyTraversal(&trav);
}
#endif

int
btree_init_traversal(struct btree *tree, struct btree_traversal *trav)
{
	if (tree == NULL)
		return -E_INVAL ;

	memset(trav, 0, sizeof(struct btree_traversal));
	trav->tree = tree;

	return 0 ;
}

char
btree_first_offset(struct btree_traversal *trav)
{
	if (trav == NULL)
		return 0 ;

	if (trav->tree->size == 0)
		return 0 ;

	if (trav->node != NULL)
		return btree_next_entry(trav);

	trav->tree->left_leaf = bt_left_leaf(trav->tree);

	trav->node = btree_read_node(trav->tree, trav->tree->left_leaf);

	if (trav->node == NULL)
		return 0 ;

	trav->pos = 1;

	trav->key = btree_key(trav->node->keys, 0, trav->tree->s_key) ;
	//trav->val = trav->node->children[0] ;
	trav->val = btree_value(trav->node->children, 0, trav->tree->s_value) ;

	return 1 ;
}

char
btree_next_entry(struct btree_traversal *trav)
{
	
	if (trav == NULL)
		return 0 ;

	if (trav->node == NULL)
		return btree_first_offset(trav);

	if (trav->pos == trav->node->keyCount)
	{
		//offset_t nextNodeOffset = trav->node->children[trav->pos];
		offset_t nextNodeOffset = *btree_value(trav->node->children, 
											  trav->pos, 
											  trav->tree->s_value);
		
		btree_destroy_node(trav->node);

		trav->node = NULL;

		if (nextNodeOffset == 0)
			return 0 ;
		
		trav->node = btree_read_node(trav->tree, nextNodeOffset);

		trav->pos = 0;
	}

	
	//trav->val = trav->node->children[trav->pos];
	trav->val = btree_value(trav->node->children, trav->pos, trav->tree->s_value) ;
	trav->key = btree_key(trav->node->keys, trav->pos, trav->tree->s_key) ;

	trav->pos++;
	return 1 ;
}

static void
__btree_pretty_print(struct btree *tree, offset_t rootOffset, int i)
{
	int j;
	struct btree_node *rootNode;

	if (rootOffset == 0) {
		cprintf("[ empty ]\n") ;
		return ;
	}
	
	rootNode = btree_read_node(tree, rootOffset);

	for (j = i; j > 0; j--)
		cprintf("    ");

	cprintf("[.");

	for (j = 0; j < rootNode->keyCount; j++) {
		const offset_t *off = btree_key(rootNode->keys, j, tree->s_key) ;
		//printf(" %ld .", *off);
		cprintf(" %ld", off[0]);
		int k = 1 ;
		for (; k < tree->s_key ; k++) {
			cprintf("|%ld", off[k]) ; 	
		}
		cprintf(" .") ;
	
	}
		//printf(" %ld .", rootNode->keys[j]);
	
	if (BTREE_IS_LEAF(rootNode))
		for (j = btree_leaf_order(rootNode) - rootNode->keyCount; j > 1; j--)
			cprintf(" _____ .");
	else
		for (j = tree->order - rootNode->keyCount; j > 1; j--)
			cprintf(" _____ .");
	
	cprintf("] - %ld\n", rootOffset);

	if (BTREE_IS_LEAF(rootNode))
	{
		btree_destroy_node(rootNode);
		//btree_unpin_node(tree, rootNode);
		return;
	}
	
	for (j = 0; j <= rootNode->keyCount; j++)
		__btree_pretty_print(tree, rootNode->children[j], i + 1);


	btree_destroy_node(rootNode);
	//btree_unpin_node(tree, rootNode);
}

void 
btree_pretty_print(struct btree *tree, offset_t rootOffset, int i)
{
	__btree_pretty_print(tree, rootOffset, i) ;
}

