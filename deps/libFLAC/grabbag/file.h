/* grabbag - Convenience lib for various routines common to several tools
 * Copyright (C) 2002-2009  Josh Coalson
 * Copyright (C) 2011-2013  Xiph.Org Foundation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

/* Convenience routines for manipulating files */

/* This .h cannot be included by itself; #include "share/grabbag.h" instead. */

#ifndef GRABAG__FILE_H
#define GRABAG__FILE_H

/* needed because of off_t */
#ifdef HAVE_CONFIG_H
#  include <config.h>
#endif

#include <sys/types.h> /* for off_t */
#include <stdio.h> /* for FILE */
#include "FLAC/ordinals.h"
#include "share/compat.h"

#endif
