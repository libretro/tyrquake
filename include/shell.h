/*
Copyright (C) 2002 Kevin Shanahan

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#ifndef SHELL_H
#define SHELL_H

#include "qtypes.h"
#include "rb_tree.h"

/*
 * String node is simply an rb_tree node using the string as the index (and
 * the only data as well).
 */
struct stree_node {
    const char *string;
    struct rb_node node;
};

/* stree_entry - Gets the stree_node ptr from the internal rb_node ptr */
#define stree_entry(ptr) container_of(ptr, struct stree_node, node)

/* For iterative tree walking */
struct stree_stack {
    struct rb_node **stack;
    int depth;
    int max_depth;
};

/*
 * We keep track of the number of entries in the tree as well as the longest
 * entry in the tree. This info is used for displaying lists on the console.
 */
struct stree_root {
    unsigned int entries;
    unsigned int maxlen;
    unsigned int minlen;
    struct rb_root root;
    struct stree_stack *stack; /* used in STree_ForEach() */
};

#define STREE_ROOT (struct stree_root) { 0, 0, -1, RB_ROOT, NULL }
#define DECLARE_STREE_ROOT(_x) \
	struct stree_root _x = { \
		.entries = 0,    \
		.maxlen = 0,     \
		.minlen = -1,    \
      		.root = RB_ROOT, \
		.stack = NULL    \
	}

void STree_AllocInit(void);
qboolean STree_Insert(struct stree_root *root, struct stree_node *node);
qboolean STree_InsertAlloc(struct stree_root *root, const char *s,
			   qboolean alloc_str);
void STree_Remove(struct stree_root *root, struct stree_node *node);
char *STree_MaxMatch(struct stree_root *root, const char *pfx);
struct stree_node *STree_Find(struct stree_root *root, const char *s);

/*
 * Scan the source tree for completions and add them into the into the
 * destination tree. Caller provides the root, added nodes are
 * allocated on the temp hunk, so STree_AllocInit needs to be done by
 * the caller.
 */
void STree_Completions(struct stree_root *out, struct stree_root *in,
		       const char *s);

/* Private helper functions for the STree_ForEach macro */
void STree_ForEach_Init__(struct stree_root *root, struct stree_node **n);
void STree_ForEach_After__(struct stree_root *root, struct stree_node **n,
			   const char *s);
qboolean STree_WalkLeft__(struct stree_root *root, struct stree_node **n);
void STree_WalkRight__(struct stree_node **n);
void STree_ForEach_Cleanup__(struct stree_root *root);

/*
 * Walk an STree root__ in order (LNR), using node__ as the iterator
 * - struct stree_root *root__
 * - struct stree_node *node__
 *
 * Note that we need to allocate a stack to walk the tree (on the zone). If
 * this fails, the program will exit with an error. This is only of size
 * 2*log(n+1), so if that allocation fails, we're in a bad situation anyway...
 *
 * STree_ForEach_After will start walking the tree from the entry _after_ the
 * one matching str__.
 *
 * NOTE - don't use a regular 'break' inside this loop, or you'll leak the
 *        allocated stack memory. Use STree_ForEach_break(root).
 */
#define STree_ForEach_After(root__, node__, str__) for (		      \
	STree_ForEach_Init__((root__), &(node__)),			      \
	({ if (str__) STree_ForEach_After__((root__), &(node__), (str__));}); \
	STree_WalkLeft__((root__), &(node__)) ;				      \
	STree_WalkRight__(&(node__)) )
#define STree_ForEach(root__, node__) \
	STree_ForEach_After(root__, node__, NULL)
#define STree_ForEach_break(root__) \
	({ STree_ForEach_Cleanup__(root__); break; })

/*
 * Set up some basic completion helpers
 * FIXME - document the API
 */

/* An rb-tree of completion strings */
struct completion {
    const char *string;			/* command name */
    unsigned long cmd_type;		/* flags for command type */
    struct rb_node rb_cmd_cache;
};

/* completion_entry - Gets the completion ptr from the internal rb_node ptr */
#define completion_entry(ptr) \
	container_of(ptr, struct completion, rb_cmd_cache)

/*
 * Command types
 *
 * There is a priority here to be compatible with id's original work. Command
 * names and cvar names are unique, but aliases can overlap. In that case and
 * alias can override a cvar, but not commands.
 */
#define CMD_COMMAND	(1UL << 0)
#define CMD_ALIAS	(1UL << 1)
#define CMD_CVAR	(1UL << 2)

/* search results end up here... FIXME - pretty ugly... */
extern const char** completions_list;

void insert_alias_completion(const char *str);
unsigned find_alias_completions(const char *str);
char *find_alias_completion(const char *str);
int alias_exists(const char *str);

void insert_command_completion(const char *str);
unsigned find_command_completions(const char *str);
char *find_command_completion(const char *str);
int command_exists(const char *str);

void insert_cvar_completion(const char *str);
unsigned find_cvar_completions(const char *str);
char *find_cvar_completion(const char *str);
int cvar_exists(const char *str);

/* Search all three completion caches combined */
unsigned find_completions(const char *str);
char *find_completion(const char *str);

#endif /* SHELL_H */
