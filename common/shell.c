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

/*
 * This whole setup is butt-ugly. Proceed with caution.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console.h"
#include "rb_tree.h"
#include "shell.h"
#include "sys.h"
#include "zone.h"

/*
 * When we want to build up temporary trees of strings for completions, file
 * listings, etc. we can use the "temp" hunk since we don't want to keep them
 * around. These allocator functions attempt to make more efficient use of the
 * hunk space by keeping the tree nodes together in blocks and allocating
 * strings right next to each other.
 */
static struct stree_node *st_node_next;
static int st_node_space;
static char *st_string_next;
static int st_string_space;

#define ST_NODE_CHUNK	2048 /* 2kB / 16B => 128 nodes */
#define ST_STRING_CHUNK	4096 /* 4k of strings together */

void
STree_AllocInit(void)
{
    /* Init the temp hunk */
    st_node_next = Hunk_TempAlloc(ST_NODE_CHUNK);
    st_node_space = ST_NODE_CHUNK;

    /* Allocate string space on demand */
    st_string_space = 0;
}

static struct stree_node *
STree_AllocNode(void)
{
    struct stree_node *ret = NULL;

    if (st_node_space < sizeof(struct stree_node)) {
	st_node_next = Hunk_TempAllocExtend(ST_NODE_CHUNK);
	st_node_space = st_node_next ? ST_NODE_CHUNK : 0;
    }
    if (st_node_space >= sizeof(struct stree_node)) {
	ret = st_node_next++;
	st_node_space -= sizeof(struct stree_node);
    }

    return ret;
}

static void *
STree_AllocString(unsigned int length)
{
    char *ret = NULL;

    if (st_string_space < length) {
	/*
	 * Note: might want to consider different allocation scheme here if we
	 * end up wasting a lot of space. E.g. if space wasted > 16, may as
	 * well use another chunk. This may cause excessive calls to
	 * Cache_FreeHigh, so maybe only do it if wasting more than that
	 * (32/64/?).
	 */
	st_string_next = Hunk_TempAllocExtend(ST_STRING_CHUNK);
	st_string_space = st_string_next ? ST_STRING_CHUNK : 0;
    }
    if (st_string_space >= length) {
	ret = st_string_next;
	st_string_next += length;
	st_string_space -= length;
    }

    return ret;
}

/*
 * Insert string node "node" into rb_tree rooted at "root"
 */
qboolean
STree_Insert(struct stree_root *root, struct stree_node *node)
{
    struct rb_node **p = &root->root.rb_node;
    struct rb_node *parent = NULL;
    struct stree_node *sn;
    unsigned int len;
    int cmp;

    while (*p) {
	parent = *p;
	sn = rb_entry(parent, struct stree_node, node);
	cmp = strcasecmp(node->string, sn->string);
	if (cmp < 0)
	    p = &(*p)->rb_left;
	else if (cmp > 0)
	    p = &(*p)->rb_right;
	else
	    return false; /* string already present */
    }
    root->entries++;
    len = strlen(node->string);
    if (len > root->maxlen)
	root->maxlen = len;
    if (len < root->minlen)
	root->minlen = len;
    rb_link_node(&node->node, parent, p);
    rb_insert_color(&node->node, &root->root);

    return true;
}

/*
 * Insert string into rb tree, allocating the string dynamically. If n
 * is NULL, the node structure is also allocated for the caller.
 * NOTE: These allocations are only on the Temp hunk.
 */
qboolean
STree_InsertAlloc(struct stree_root *root, const char *s,
		  struct stree_node *n)
{
    qboolean ret = false;

    if (!n)
	n = STree_AllocNode();
    if (n)
	n->string = STree_AllocString(strlen(s) + 1);
    if (n && n->string) {
	strcpy(n->string, s);
	ret = STree_Insert(root, n);
    }

    return ret;
}

/* STree_MaxMatch helper */
static int
ST_node_match(struct rb_node *n, const char *str, int min_match, int max_match)
{
    struct stree_node *sn;

    if (n) {
	max_match = ST_node_match(n->rb_left, str, min_match, max_match);

	/* How much does this node match */
	sn = rb_entry(n, struct stree_node, node);
	while (max_match > min_match) {
	    if (!strncasecmp(str, sn->string, max_match))
		break;
	    max_match--;
	}

	max_match = ST_node_match(n->rb_right, str, min_match, max_match);
    }

    return max_match;
}

/*
 * Given a prefix, return the maximum common prefix of all other strings in
 * the tree which match the given prefix.
 */
char *
STree_MaxMatch(struct stree_root *root, const char *pfx)
{
    int max_match, min_match, match;
    struct rb_node *n;
    struct stree_node *sn;
    char *result = NULL;

    /* Can't be more than the shortest string */
    max_match = root->minlen;
    min_match = strlen(pfx);

    n = root->root.rb_node;
    sn = rb_entry(n, struct stree_node, node);

    if (root->entries == 1) {
	match = strlen(sn->string);
	result = Z_Malloc(match + 2);
	if (result) {
	    strncpy(result, sn->string, match);
	    result[match] = ' ';
	    result[match + 1] = 0;
	}
    } else if (root->entries > 1) {
	match = ST_node_match(n, sn->string, min_match, max_match);
	result = Z_Malloc(match + 1);
	if (result) {
	    strncpy(result, sn->string, match);
	    result[match] = 0;
	}
    }

    return result;
}

const char **completions_list = NULL;
static int num_completions = 0;

static struct rb_root completions = RB_ROOT;

/*
 * FIXME: document assumptions about args; see caller below
 */
static struct completion *
rb_find_exact_r(struct rb_node *n, const char *str, unsigned long type)
{
    if (n) {
	struct completion *c, *ret = NULL; /* FIXME - null right here? */

	c = rb_entry(n, struct completion, rb_cmd_cache);
	if (!strcasecmp(str, c->string)) {
	    ret = rb_find_exact_r(n->rb_left, str, type);
	    if (!ret && (c->cmd_type & type))
		ret = c;
	    if (!ret)
		ret = rb_find_exact_r(n->rb_right, str, type);
	}
	return ret;
    }
    return NULL;
}


/*
 * Find exact string
 */
static int
rb_find_exact(const char *str, unsigned long type, struct rb_root *root)
{
    struct rb_node *n = root->rb_node;
    struct completion *c;
    int cmp;

    /* Do the first part iteratively */
    while (n) {
	c = rb_entry(n, struct completion, rb_cmd_cache);
	cmp = strcasecmp(str, c->string);
	if (cmp < 0)
	    n = n->rb_left;
	else if (cmp > 0)
	    n = n->rb_right;
	else
	    break;
    }
    /* now take care of the type matching */
    return (rb_find_exact_r(n, str, type) != NULL);
}


/*
 * str, type - the insertion key (only use one type 'flag' here)
 * root - root of the tree being inserted into
 * node - the node being inserted
 */
static struct completion *
rb_insert_completion__(const char *str, unsigned long type,
		       struct rb_root *root, struct rb_node *node)
{
    struct rb_node **p = &root->rb_node;
    struct rb_node *parent = NULL;
    struct completion *c;
    int cmp;

    while (*p) {
	parent = *p;
	c = rb_entry(parent, struct completion, rb_cmd_cache);

	cmp = strcasecmp(str, c->string);
	if (cmp < 0)
	    p = &(*p)->rb_left;
	else if (cmp > 0)
	    p = &(*p)->rb_right;
	else if (type < c->cmd_type)
	    p = &(*p)->rb_left;
	else if (type > c->cmd_type)
	    p = &(*p)->rb_right;
	else
	    return c; /* Already match in cache */
    }
    rb_link_node(node, parent, p);

    return NULL;
}

static void
rb_insert_completion(const char *str, unsigned long type, struct rb_root *root)
{
    struct completion *c, *ret;

    c = Z_Malloc(sizeof(struct completion));
    c->string = str;
    c->cmd_type = type;

    ret = rb_insert_completion__(str, type, root, &c->rb_cmd_cache);

    /* FIXME: insertion failure is important ; return error code and handle */
    if (ret == NULL)
	rb_insert_color(&c->rb_cmd_cache, root);
    else {
	Con_DPrintf("** Attempted to insert duplicate completion: %s\n", str);
	Z_Free(c);
    }
}

/*
 * FIXME: when I'm not so tired, I'll make this more efficient...
 */
static unsigned
rb_count_completions_r(struct rb_node *n, const char *str, unsigned long type)
{
    unsigned cnt = 0;

    if (n) {
	struct completion *c = rb_entry(n, struct completion, rb_cmd_cache);
	int cmp = strncasecmp(str, c->string, strlen(str));
	if (cmp <= 0)
	    cnt += rb_count_completions_r(n->rb_left, str, type);
	if (!cmp && (c->cmd_type & type))
	    cnt++;
	if (cmp >= 0)
	    cnt += rb_count_completions_r(n->rb_right, str, type);
    }
    return cnt;
}

static unsigned
rb_count_completions(struct rb_node *n, const char *str, unsigned long type)
{
    /* do the first part iteratively */
    while (n) {
	struct completion *c = rb_entry(n, struct completion, rb_cmd_cache);
	int cmp = strncasecmp(str, c->string, strlen(str));
	if (cmp < 0)
	    n = n->rb_left;
	else if (cmp > 0)
	    n = n->rb_right;
	else
	    break;
    }
    return rb_count_completions_r(n, str, type);
}

static void
rb_find_completions_r(struct rb_node *n, const char *str, unsigned long type)
{
    if (n) {
	struct completion *c = rb_entry(n, struct completion, rb_cmd_cache);
	int cmp = strncasecmp(str, c->string, strlen(str));
	if (cmp <= 0)
	    rb_find_completions_r(n->rb_left, str, type);
	if (!cmp && (c->cmd_type & type))
	    completions_list[num_completions++] = c->string;
	if (cmp >= 0)
	    rb_find_completions_r(n->rb_right, str, type);
    }
}

static unsigned
rb_find_completions(const char *str, unsigned long type, struct rb_root *root)
{
    struct rb_node *n;
    unsigned cnt;

    n = root->rb_node;
    cnt = rb_count_completions(n, str, type);

    if (cnt) {
	if (completions_list)
	    free(completions_list);

	completions_list = malloc(cnt * sizeof(char *));
	num_completions = 0;
	rb_find_completions_r(n, str, type);

	if (num_completions != cnt)
	    Con_DPrintf("**** WARNING: rb completions counts don't match!\n");
    }
    return cnt;
}

/*
 * Given the partial string 'str', find all strings in the tree that have
 * this prefix. Return the common prefix of all those strings, which may
 * be longer that the original str.
 *
 * Returned str is allocated on the zone, so caller to Z_Free it after use.
 * If no matches found, return null;
 */
static char *
rb_find_completion(const char *str, unsigned long type, struct rb_root *root)
{
    int n, i, max_match, len;
    char *ret;

    n = rb_find_completions(str, type, root);
    if (!n)
	return NULL;

    /*
     * What is the most that could possibly match?
     * i.e. shortest string in the list
     */
    max_match = strlen(completions_list[0]);
    for (i = 1; i < n; ++i) {
	len = strlen(completions_list[i]);
	if (len < max_match)
	    max_match = len;
    }

    /*
     * Check if we can match max_match chars. If not, reduce by one and
     * try again.
     */
    while (max_match > strlen(str)) {
	for (i = 1; i < n; ++i) {
	    if (strncasecmp(completions_list[0], completions_list[i],
			    max_match)) {
		max_match--;
		break;
	    }
	}
	if (i == n)
	    break;
    }

    ret = Z_Malloc(max_match + 1);
    strncpy(ret, completions_list[0], max_match);
    ret[max_match] = '\0';

    return ret;
}




/* Command completions */

void
insert_command_completion(const char *str)
{
    rb_insert_completion(str, CMD_COMMAND, &completions);
}

unsigned
find_command_completions(const char *str)
{
    return rb_find_completions(str, CMD_COMMAND, &completions);
}

char *
find_command_completion(const char *str)
{
    return rb_find_completion(str, CMD_COMMAND, &completions);
}

int
command_exists(const char *str)
{
    return rb_find_exact(str, CMD_COMMAND, &completions);
}

/* Alias completions */

void
insert_alias_completion(const char *str)
{
    rb_insert_completion(str, CMD_ALIAS, &completions);
}

unsigned
find_alias_completions(const char *str)
{
    return rb_find_completions(str, CMD_ALIAS, &completions);
}

char *
find_alias_completion(const char *str)
{
    return rb_find_completion(str, CMD_ALIAS, &completions);
}

int
alias_exists(const char *str)
{
    return rb_find_exact(str, CMD_ALIAS, &completions);
}


/* Cvar completions */

void
insert_cvar_completion(const char *str)
{
    rb_insert_completion(str, CMD_CVAR, &completions);
}

unsigned
find_cvar_completions(const char *str)
{
    return rb_find_completions(str, CMD_CVAR, &completions);
}

char *
find_cvar_completion(const char *str)
{
    return rb_find_completion(str, CMD_CVAR, &completions);
}

int
cvar_exists(const char *str)
{
    return rb_find_exact(str, CMD_CVAR, &completions);
}

/* ------------------------------------------------------------------------ */

/*
 * Find all the completions for a string, looking at commands, cvars and
 * aliases.
 */
unsigned
find_completions(const char *str)
{
    return rb_find_completions(str, CMD_COMMAND | CMD_CVAR | CMD_ALIAS,
			       &completions);
}


/*
 * For tab completion. Checks cmds, cvars and aliases
 */
char *find_completion(const char *str)
{
    return rb_find_completion(str, CMD_COMMAND | CMD_CVAR | CMD_ALIAS,
			      &completions);
}
