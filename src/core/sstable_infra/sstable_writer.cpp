#include "core/sstable_infra/sstable_writer.h"

Status SSTableWriter::write(SSTable& sstable)
{
    return sstable.write();
}
