/*
Copyright (C) 1996-1997 Id Software, Inc.

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

#include "common.h"
#include "net.h"

int
NET_AddrCompare(const netadr_t *addr1, const netadr_t *addr2)
{
    if (addr1->ip.l != addr2->ip.l || addr1->port != addr2->port)
	return -1;

    return 0;
}

int
NET_GetSocketPort(const netadr_t *addr)
{
    return (unsigned short)BigShort(addr->port);
}

int
NET_SetSocketPort(netadr_t *addr, int port)
{
    addr->port = (unsigned short)BigShort(port);
    return 0;
}
