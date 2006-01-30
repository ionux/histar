/**
 * @file btree_delete.c Deletion functions
 * 
 * $Id: btree_delete.c,v 1.5 2002/04/07 18:29:40 chipx86 Exp $
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
#include <inc/stdio.h>

static char
__removeKey(struct btree *tree, 
			struct btree_node *rootNode, 
			const uint64_t *key,
			offset_t *filePos)
{
	int i;
	
	for (i = 0;
		 i < rootNode->keyCount && 
		 btree_keycmp(btree_key(rootNode->keys, i, tree->s_key), key, tree->s_key) < 0;
		 i++)
		;

	if (BTREE_IS_LEAF(rootNode) && i < rootNode->keyCount &&
		btree_keycmp(btree_key(rootNode->keys, i, tree->s_key), key, tree->s_key) == 0)
	{
		*filePos = rootNode->children[i];

		for (; i < rootNode->keyCount - 1; i++)
		{
			//rootNode->keys[i]     = rootNode->keys[i + 1];
			//btree_keycpy(&rootNode->keys[i], &rootNode->keys[i + 1], tree->s_key) ;
			btree_keycpy(btree_key(rootNode->keys, i, tree->s_key), 
						 btree_key(rootNode->keys, i + 1, tree->s_key), 
						 tree->s_key) ;
			rootNode->children[i] = rootNode->children[i + 1];
		}

		//rootNode->keys[i]         = 0;
		//btree_keyset(&rootNode->keys[i], 0, tree->s_key) ;
		btree_keyset(btree_key(rootNode->keys, i, tree->s_key), 0, tree->s_key) ;
		rootNode->children[i]     = rootNode->children[i + 1];
		rootNode->children[i + 1] = 0;

		rootNode->keyCount--;

		BTB_SET_DIRTY(rootNode->block);

		btree_write_node(rootNode);

		return 1;
	}

	return 0;
}

static void
__removeKey2(struct btree *tree, 
			 struct btree_node *rootNode, 
			 int index)
{
	int i;

	for (i = index; i < rootNode->keyCount - 1; i++)
	{
		//rootNode->keys[i]     = rootNode->keys[i + 1];
		btree_keycpy(btree_key(rootNode->keys, i, tree->s_key), 
					 btree_key(rootNode->keys, i + 1, tree->s_key), 
					 tree->s_key) ;
		rootNode->children[i] = rootNode->children[i + 1];
	}

	//rootNode->keys[i]         = 0;
	btree_keyset(btree_key(rootNode->keys, i, tree->s_key), 0, tree->s_key) ;
	rootNode->children[i]     = rootNode->children[i + 1];
	rootNode->children[i + 1] = 0;

	rootNode->keyCount--;

	BTB_SET_DIRTY(rootNode->block);

	btree_write_node(rootNode);
}

static char
__borrowRight(struct btree *tree, 
			  struct btree_node *rootNode, 
			  struct btree_node *prevNode, 
			  int div)
{
	struct btree_node *node;

	if (div >= prevNode->keyCount)
		return 0;

	node = bt_read_node(tree, prevNode->children[div + 1]);

	if (BTREE_IS_LEAF(node) && node->keyCount > tree->min_leaf)
	{
		rootNode->children[rootNode->keyCount + 1] =
			rootNode->children[(int)rootNode->keyCount];
		
		//rootNode->keys[(int)rootNode->keyCount]     = node->keys[0] ;
		btree_keycpy(btree_key(rootNode->keys, (int)rootNode->keyCount, tree->s_key), 
					 btree_key(node->keys, 0, tree->s_key), 
					 tree->s_key) ;
		rootNode->children[(int)rootNode->keyCount] = node->children[0];

		//prevNode->keys[div] = rootNode->keys[(int)rootNode->keyCount];
		btree_keycpy(btree_key(prevNode->keys, div, tree->s_key),  
					 btree_key(rootNode->keys, (int)rootNode->keyCount, tree->s_key),
					 tree->s_key) ;
	}
	else if (!BTREE_IS_LEAF(node) && node->keyCount > tree->min_intrn)
	{
		//rootNode->keys[(int)rootNode->keyCount] = prevNode->keys[div];
		btree_keycpy(btree_key(rootNode->keys, (int)rootNode->keyCount, tree->s_key),  
					 btree_key(prevNode->keys, div, tree->s_key),  
					 tree->s_key) ;

		//prevNode->keys[div]                         = node->keys[0] ;
		btree_keycpy(btree_key(prevNode->keys, div, tree->s_key), 
					 btree_key(node->keys, 0, tree->s_key), 
					 tree->s_key) ;

		rootNode->children[rootNode->keyCount + 1] = node->children[0];
	}
	else
	{
		btree_destroy_node(node);

		return 0;
	}

	BTB_SET_DIRTY(rootNode->block);
	BTB_SET_DIRTY(prevNode->block);

	rootNode->keyCount++;

	__removeKey2(tree, node, 0);

	btree_destroy_node(node);
	
	return 1;
}

static char
__borrowLeft(struct btree *tree, 
			 struct btree_node *rootNode, 
			 struct btree_node *prevNode, 
			 int div)
{
	int i;
	struct btree_node *node;

	if (div == 0)
		return 0;

	node = bt_read_node(tree, prevNode->children[div - 1]);

	if (BTREE_IS_LEAF(node) && node->keyCount > tree->min_leaf)
	{
		for (i = rootNode->keyCount; i > 0; i--)
		{
			//rootNode->keys[i]         = rootNode->keys[i - 1];
			btree_keycpy(btree_key(rootNode->keys, i, tree->s_key), 
						 btree_key(rootNode->keys, i - 1, tree->s_key),
						 tree->s_key) ;
			rootNode->children[i + 1] = rootNode->children[i];
		}

		rootNode->children[1] = rootNode->children[0];
		//rootNode->keys[0]     = node->keys[node->keyCount - 1];
		//btree_keycpy(&rootNode->keys[0], &node->keys[node->keyCount - 1], tree->s_key) ;
		btree_keycpy(btree_key(rootNode->keys, 0, tree->s_key), 
					 btree_key(node->keys, node->keyCount - 1, tree->s_key),
					 tree->s_key) ;
		rootNode->children[0] = node->children[node->keyCount - 1];

		rootNode->keyCount++;

		//prevNode->keys[div - 1]     = node->keys[node->keyCount - 2];
		btree_keycpy(btree_key(prevNode->keys, div - 1, tree->s_key),  
					 btree_key(node->keys, node->keyCount - 2, tree->s_key),  
					 tree->s_key) ;

		node->children[node->keyCount - 1] =
			node->children[(int)node->keyCount];

		node->children[(int)node->keyCount] = 0;

		//node->keys[node->keyCount - 1]     = 0;
		btree_keyset(btree_key(node->keys, node->keyCount - 1, tree->s_key), 
					 0, 
					 tree->s_key) ;
	}
	else if (!BTREE_IS_LEAF(node) && node->keyCount > tree->min_intrn)
	{
		for (i = rootNode->keyCount; i > 0; i--)
		{
			//rootNode->keys[i]         = rootNode->keys[i - 1];
			btree_keycpy(btree_key(rootNode->keys, i, tree->s_key), 
						 btree_key(rootNode->keys, i - 1, tree->s_key),
						 tree->s_key) ;
			rootNode->children[i + 1] = rootNode->children[i];
		}

		rootNode->children[1] = rootNode->children[0];
		//rootNode->keys[0]     = prevNode->keys[div - 1];
		btree_keycpy(btree_key(rootNode->keys, 0, tree->s_key),  
					 btree_key(prevNode->keys, div - 1, tree->s_key),  
					 tree->s_key) ;
		rootNode->children[0] = node->children[(int)node->keyCount];

		rootNode->keyCount++;

		//prevNode->keys[div - 1]     = node->keys[node->keyCount - 1];
		btree_keycpy(btree_key(prevNode->keys, div - 1, tree->s_key), 
					 btree_key(node->keys, node->keyCount - 1, tree->s_key),  
					 tree->s_key) ;
		
		node->children[(int)node->keyCount] = 0;

		//node->keys[node->keyCount - 1]     = 0;
		btree_keyset(btree_key(node->keys, node->keyCount - 1, tree->s_key), 0, tree->s_key) ;
	}
	else
	{
		btree_destroy_node(node);
		
		return 0;
	}

	node->keyCount--;

	BTB_SET_DIRTY(rootNode->block);
	BTB_SET_DIRTY(prevNode->block);
	BTB_SET_DIRTY(node->block);

	btree_write_node(node);
	btree_destroy_node(node);

	return 1;
}

static char __attribute__((noinline))
__mergeNode(struct btree *tree, 
			struct btree_node *rootNode, 
			struct btree_node *prevNode, 
			int div)
{
	int i, j;
	struct btree_node *node;

	/* Try to merge the node with its left sibling. */
	if (div > 0)
	{
		node = bt_read_node(tree, prevNode->children[div - 1]);
		i    = node->keyCount;

		if (!BTREE_IS_LEAF(rootNode))
		{
			
			//node->keys[i]     = prevNode->keys[div - 1];
			btree_keycpy(btree_key(node->keys, i, tree->s_key), 
						 btree_key(prevNode->keys, div - 1, tree->s_key), 
						 tree->s_key) ;
			node->keyCount++;
			
			i++;
		}

		for (j = 0; j < rootNode->keyCount; j++, i++)
		{
			//node->keys[i]     = rootNode->keys[j];
			btree_keycpy(btree_key(node->keys, i, tree->s_key), 
						 btree_key(rootNode->keys, j, tree->s_key), 
						 tree->s_key) ;
			node->children[i] = rootNode->children[j];
			node->keyCount++;
		}

		node->children[i] = rootNode->children[j];

		BTB_SET_DIRTY(node->block);

		btree_write_node(node);
		
		prevNode->children[div] = node->block.offset;

		BTB_SET_DIRTY(prevNode->block);

		btree_erase_node(rootNode);
		__removeKey2(tree, prevNode, div - 1);
		
		btree_write_node(node);
		btree_write_node(prevNode);

		btree_destroy_node(node);
		
		
	}
	else
	{
		/* Must merge the node with its right sibling. */
		node = bt_read_node(tree, prevNode->children[div + 1]);
		i    = rootNode->keyCount;

		if (!BTREE_IS_LEAF(rootNode))
		{
			//rootNode->keys[i]     = prevNode->keys[div];
			btree_keycpy(btree_key(rootNode->keys, i, tree->s_key), 
						 btree_key(prevNode->keys, div, tree->s_key),  
						 tree->s_key) ;
			rootNode->keyCount++;
			
			i++;
		}

		for (j = 0; j < node->keyCount; j++, i++)
		{
			//rootNode->keys[i]     = node->keys[j];
			btree_keycpy(btree_key(rootNode->keys, i, tree->s_key), 
						 btree_key(node->keys, j, tree->s_key),
						 tree->s_key) ;
			rootNode->children[i] = node->children[j];
			rootNode->keyCount++;
		}
		
		rootNode->children[i]       = node->children[j];
		prevNode->children[div + 1] = rootNode->block.offset;

		BTB_SET_DIRTY(rootNode->block);
		BTB_SET_DIRTY(prevNode->block);

		btree_erase_node(node);

		__removeKey2(tree, prevNode, div);
		
		btree_write_node(prevNode);
		btree_write_node(rootNode);
	}

	//btree_write_node(node);
	//btree_write_node(prevNode);
	//btree_write_node(rootNode);

	//btree_destroy_node(node);

	return 1;
}

static char
__delete(struct btree *tree, 
		 offset_t rootOffset, 
		 struct btree_node *prevNode,
		 const uint64_t *key, 
		 int index, 
		 offset_t *filePos, 
		 char *merged)
{
	char success = 0;
	struct btree_node *rootNode;

	rootNode = bt_read_node(tree, rootOffset);

	if (BTREE_IS_LEAF(rootNode))
	{
		success = __removeKey(tree, rootNode, key, filePos);
	}
	else
	{
		int i;
		
		for (i = 0;
			 i < rootNode->keyCount && 
			 btree_keycmp(btree_key(rootNode->keys, i, tree->s_key), key, tree->s_key) < 0;
			 i++)
			;

		success = __delete(tree, rootNode->children[i], rootNode, key, i,
						   filePos, merged);
	}

	if (success == 0)
	{
		btree_destroy_node(rootNode);
		
		return 0;
	}
	else if ((rootNode->block.offset == tree->root) ||
			 (BTREE_IS_LEAF(rootNode)  && rootNode->keyCount >= tree->min_leaf) ||
			 (!BTREE_IS_LEAF(rootNode) && rootNode->keyCount >= tree->min_intrn))
	{
		btree_destroy_node(rootNode);
		
		return 1;
	}
	else
	{
		if (__borrowRight(tree, rootNode, prevNode, index) ||
			__borrowLeft(tree, rootNode, prevNode, index))
		{
			*merged = 0;
			btree_write_node(rootNode);
			btree_write_node(prevNode);
		}
		else
		{
			*merged = 1;
			__mergeNode(tree, rootNode, prevNode, index);
		}
		//btree_write_node(rootNode);
		//btree_write_node(prevNode);

	}

	btree_destroy_node(rootNode);
	
	return 1;
}

int64_t 
btree_delete(struct btree *tree, const uint64_t *key)
{
	int i;
	offset_t filePos;
	char merged, success;
	struct btree_node *rootNode;

	if (tree == NULL || key == 0)
		return 0;

	filePos = 0;
	merged  = 0;
	success = 0;

	// XXX: remove?
	tree->root     = bt_root_node(tree);
	tree->left_leaf = bt_left_leaf(tree);
	tree->size     = bt_tree_size(tree);

	/* Read in the root node. */
	rootNode = bt_read_node(tree, tree->root);
	
	//cprintf("btree_delete start\n") ;

	for (i = 0;
		 i < rootNode->keyCount && 
		 btree_keycmp(btree_key(rootNode->keys, i, tree->s_key), key, tree->s_key) < 0;
		 i++)
		;

	success = __delete(tree, tree->root, NULL, key, i, &filePos, &merged);

	if (success == 0)
	{
		btree_destroy_node(rootNode);
		btree_release_nodes(tree) ;
		//cprintf("btree_delete stop\n") ;
		return 0;
	}
	
	bt_tree_size_is(tree, tree->size - 1);


	if (BTREE_IS_LEAF(rootNode) && rootNode->keyCount == 0)
	{
		btree_root_node_is(tree, 0);
		btree_erase_node(rootNode);
		btree_release_nodes(tree) ;
		//cprintf("btree_delete stop\n") ;
		return filePos ;
	}
	else if (merged == 1 && rootNode->keyCount == 0)
	{
		struct btree_node *tempNode;
		
		btree_root_node_is(tree, rootNode->children[0]);

		tempNode = bt_read_node(tree, tree->root);

		BTB_SET_DIRTY(tempNode->block);

		btree_write_node(tempNode);
		btree_destroy_node(tempNode);

		btree_erase_node(rootNode);
		btree_release_nodes(tree) ;
		//cprintf("btree_delete stop\n") ;
		return filePos ;
	}
	btree_destroy_node(rootNode);
	btree_release_nodes(tree) ;
	//cprintf("btree_delete stop\n") ;
	return filePos;
}



