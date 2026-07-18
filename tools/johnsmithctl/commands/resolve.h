#pragma once
#include "../client/client.h"

bool CmdResolve(Client& client, const char* moduleName, const char* exportName);
bool ResolveSelfTest();
