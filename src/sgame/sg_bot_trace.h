/*
===========================================================================

Unvanquished GPL Source Code
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of the Unvanquished GPL Source Code (Unvanquished Source Code).

Unvanquished is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Unvanquished is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Unvanquished; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

===========================================================================
*/

#ifndef SG_BOT_TRACE_H_
#define SG_BOT_TRACE_H_

#include "sg_bot_ai.h"

botTraceDescriptor_t G_BotBuildTraceDescriptor( gentity_t *self, AINodeStatus_t status );
void G_BotResetTraceCache( botMemory_t& memory );
void G_BotTraceTransition( gentity_t *self, AINodeStatus_t status );

#endif  // SG_BOT_TRACE_H_
