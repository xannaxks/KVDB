#include "arena.h"
#include "status.h"

class Driver
{
private:
	virtual void destroy() = 0;

	class InorderIterator
	{
	public:
		virtual bool has_next() = 0;
		virtual void* next() = 0;
	};

public:
	virtual ::Status insert(const InternalRecord& entry) = 0;
	virtual Result<std::optional<InternalRecord>> find_latest_by_key(ArenaEntry key) const = 0;
	virtual bool validate() const = 0;
	virtual std::size_t approximate_memory_usage() const = 0;
	virtual bool empty() const noexcept = 0;

	/** @brief Appends all records to @p out in internal-key order. */
	virtual void dump_inorder(std::vector<InternalRecord>& out) const = 0;
};