#ifndef CGAME_ROCKET_LUA_DATA_REQUEST_H_
#define CGAME_ROCKET_LUA_DATA_REQUEST_H_

#include "shared/bg_lua.h"

// Lua API: cgame.Request(name, params):Then(onSuccess, onError)
void CG_Rocket_RegisterDataRequest( lua_State *L );
void CG_Rocket_HandleDataReply();
void CG_Rocket_HandleDataChunk();
void CG_Rocket_ClearDataRequests();
void CG_Rocket_RegisterPushCommands( lua_State *L );
void CG_Rocket_PushCommandsReady();
void CG_Rocket_HandlePushCommand();
void CG_Rocket_ResetPushCommands();
void CG_Rocket_ClearPushCommands();

#endif  // CGAME_ROCKET_LUA_DATA_REQUEST_H_
