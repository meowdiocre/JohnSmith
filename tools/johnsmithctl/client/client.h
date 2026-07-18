#pragma once
#include <cstdint>
#include "../common/types.h"

class Client {
public:
    explicit Client(const Protocol& protocol);
    ~Client();

    bool valid() const;
    bool Register();
    bool Issue(Command command, const std::uint64_t args[4], std::uint64_t* result);
    HypercallPage* page();

private:
    Protocol protocol_;
    HypercallPage* page_;
};
