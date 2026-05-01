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

#include "net.h"
#include "net_dgrm.h"
#include "net_loop.h"
#include "net_wins.h"

#include "quakedef.h"

/* Field order must match net_driver_t / net_landriver_t in net.h.
 * Designated initializers are C99; we keep these positional with
 * trailing comments so the layout is self-documenting under C89. */
net_driver_t net_drivers[] = {
    {
	"Loopback",			/* name                       */
	false,				/* initialized                */
	Loop_Init,			/* Init                       */
	Loop_Listen,			/* Listen                     */
	Loop_SearchForHosts,		/* SearchForHosts             */
	Loop_Connect,			/* Connect                    */
	Loop_CheckNewConnections,	/* CheckNewConnections        */
	Loop_GetMessage,		/* QGetMessage                */
	Loop_SendMessage,		/* QSendMessage               */
	Loop_SendUnreliableMessage,	/* SendUnreliableMessage      */
	Loop_CanSendMessage,		/* CanSendMessage             */
	Loop_CanSendUnreliableMessage,	/* CanSendUnreliableMessage   */
	Loop_Close,			/* Close                      */
	Loop_Shutdown,			/* Shutdown                   */
	0				/* controlSock                */
    }, {
	"Datagram",			/* name                       */
	false,				/* initialized                */
	Datagram_Init,			/* Init                       */
	Datagram_Listen,		/* Listen                     */
	Datagram_SearchForHosts,	/* SearchForHosts             */
	Datagram_Connect,		/* Connect                    */
	Datagram_CheckNewConnections,	/* CheckNewConnections        */
	Datagram_GetMessage,		/* QGetMessage                */
	Datagram_SendMessage,		/* QSendMessage               */
	Datagram_SendUnreliableMessage,	/* SendUnreliableMessage      */
	Datagram_CanSendMessage,	/* CanSendMessage             */
	Datagram_CanSendUnreliableMessage,/* CanSendUnreliableMessage */
	Datagram_Close,			/* Close                      */
	Datagram_Shutdown,		/* Shutdown                   */
	0				/* controlSock                */
    }
};

int net_numdrivers = 2;

net_landriver_t net_landrivers[] = {
    {
	"Winsock TCPIP",		/* name                       */
	false,				/* initialized                */
	0,				/* controlSock                */
	WINS_Init,			/* Init                       */
	WINS_Shutdown,			/* Shutdown                   */
	WINS_Listen,			/* Listen                     */
	WINS_OpenSocket,		/* OpenSocket                 */
	WINS_CloseSocket,		/* CloseSocket                */
	WINS_CheckNewConnections,	/* CheckNewConnections        */
	WINS_Read,			/* Read                       */
	WINS_Write,			/* Write                      */
	WINS_Broadcast,			/* Broadcast                  */
	WINS_GetSocketAddr,		/* GetSocketAddr              */
	WINS_GetNameFromAddr,		/* GetNameFromAddr            */
	WINS_GetAddrFromName,		/* GetAddrFromName            */
	WINS_GetDefaultMTU		/* GetDefaultMTU              */
    }
};

int net_numlandrivers = 1;
