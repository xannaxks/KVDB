#include "status.h"
#include <memory>
#include <optional>

class KVDB
{
public:
	static Result<std::unique_ptr<KVDB>> open(const KVDBoptions& options);

	template<T, S>
	Status put(const T& key, const S& value);

	template<T>
	Result<std::optional<T>> get(const T& key);

	template<T>
	Status remove(const T& key);

	Status flush();
	template<T, S>
	Status compact_range(const T& begin, const S& end);

	Status close();

private:
	KVDB() = default;
};