#include "sstable_writer.h"

Status SSTableWriter::write(SSTable& sstable)
{
    return sstable.write();
}
