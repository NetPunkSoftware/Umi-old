#pragma once

#include "network/queues/queue.hpp"


// Basically this queue does nothing, whatever the packer does
// it just relays

template <typename Packer>
class reliable_queue : public Packer
{
public:
    using Packer::Packer;
};
