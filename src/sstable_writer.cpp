class SSTableWriter
{
public:
	SSTableWriter();

	static Status write(SSTable& sstable);
	static Status write(MemTable& mem_table);
	static Status write(std::vector<InternalRecord>& records);
};
SSTableWriter::SSTableWriter()
{
}
Status SSTableWriter::write(SSTable& sstable)
{
	return sstable.write();
}