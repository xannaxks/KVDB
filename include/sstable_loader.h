
class SSTableLoader
{
public:
	SSTableLoader();

	static Result<SSTable> load(std::filesystem::path& path, Arena& arena);
};
