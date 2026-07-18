#pragma once
#include "client/client.h"
#include "loader/loader.h"
#include "loader/dse.h"

int RunRepl(Client& client, const Protocol& protocol, std::uint32_t cpu);
