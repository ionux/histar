#include <lib/btree/btree.h>
#include <inc/types.h>
#include <inc/stdio.h>
#include <inc/assert.h>

struct btree_node *
btree_new_node(struct btree *tree)
{
	struct btree_node *node ; 
	if (tree->manager.alloc(tree, &node, tree->manager.arg) < 0)
		panic("btree_new_node: unable to alloc node") ;
		
	return node;
}

void
btree_destroy_node(struct btree_node * node)
{
	if (node == NULL)
		return ;
		
	//struct btree *tree = node->tree ;
	
	// XXX: fix the pin interface thing...
	/*
	if (tree && tree->mm)
		tree->mm->pin_is(tree->mm->arg, node->block.offset, 0) ;
	*/
}

struct btree_node *
bt_read_node(struct btree *tree, offset_t offset)
{
	struct btree_node *n ;
	
	if (tree->manager.node && offset) {
		if (tree->manager.node(tree, offset, &n, tree->manager.arg) == 0)
			return n ;
	}
	
	panic("btree_read_node: unable to read node %ld\n", offset) ;
	
	return NULL ;
}

offset_t
btree_write_node(struct btree_node *node)
{
	if (node == NULL)
		return 0;
	if (node->tree == NULL)
		return 0;

	// XXX: check dirty bit

	struct btree *tree = node->tree ;
	
	assert(tree) ;
	
	if (tree->manager.write(node, tree->manager.arg) == 0)
		return node->block.offset;
		
	panic("btree_write_node: unable to write node %ld", 
		  node->block.offset) ;
	return 0 ;
}

void
btree_erase_node(struct btree_node *node)
{
	struct btree *tree = node->tree ;
	
	if (tree->manager.free)
		tree->manager.free(tree->manager.arg, node->block.offset) ;
}
