#pragma once

#include "sstable.h"
#include "status.h"

class SSTableWriter
{
public:
    [[nodiscard]] static Status write(SSTable& sstable);
};
