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
 * We keep track of the number of entries in the tree as well as the longest
 * entry in the tree. This info is used for displaying lists on the console.
 */
struct stree_root {
    unsigned int entries;
    unsigned int maxlen;
    unsigned int minlen;
    struct rb_root root;
};

#define STREE_ROOT (struct stree_root) { 0, 0, -1, RB_ROOT }
#define DECLARE_STREE_ROOT(_x) \
	struct stree_root _x = { \
		.entries = 0,    \
		.maxlen = 0,     \
		.minlen = -1,    \
      		.root = RB_ROOT  \
	}

/*
 * String node is simply an rb_tree node using the string as the index (and
 * the only data as well).
 */
struct stree_node {
    char *string;
    struct rb_node node;
};

/* stree_entry - Gets the stree_node ptr from the internal rb_node ptr */
#define stree_entry(ptr) container_of(ptr, struct stree_node, node)

void STree_AllocInit(void);
qboolean STree_Insert(struct stree_root *root, struct stree_node *node);
qboolean STree_InsertAlloc(struct stree_root *root, const char *s,
			   struct stree_node *n);
void STree_Remove(struct stree_root *root, struct stree_node *node);
char *STree_MaxMatch(struct stree_root *root, const char *pfx);
struct stree_node *STree_Find(struct stree_root *root, const char *s);
int STree_MaxDepth(struct stree_root *root);

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
