#include "mem_table.h"

#include <mutex>
#include <new>
#include <optional>
#include <utility>

MemTable::MemTable()
    : mutable_table_(std::make_shared<RBTree>())
{
}

Status MemTable::apply(const InternalRecord& entry)
{
    //std::unique_lock lock(mutex_);
    return mutable_table_->insert(entry);
}

Status MemTable::put(
    ArenaEntry key,
    ArenaEntry value,
    std::uint64_t sequence_number
)
{
    return apply(InternalRecord(
        key,
        value,
        Type::Put,
        sequence_number
    ));
}

Status MemTable::remove(
    ArenaEntry key,
    std::uint64_t sequence_number
)
{
    return apply(InternalRecord(
        key,
        ArenaEntry{},
        Type::Tombstone,
        sequence_number
    ));
}

Result<std::optional<InternalRecord>> MemTable::get(const ArenaEntry& key) const
{
    //std::shared_lock lock(mutex_);

    Result<std::optional<InternalRecord>> latest =
        mutable_table_->find_latest_by_key(key);

    if (!latest.is_ok())
        return Result<std::optional<InternalRecord>>::fail(std::move(latest.status));

    // Comparing sequence numbers makes the lookup correct even if recovery or
    // tests insert records out of generation order. In the normal monotonic
    // sequence-number case, the first hit is already the newest one.
    for (auto it = immutable_tables_.rbegin();
        it != immutable_tables_.rend();
        ++it)
    {
        Result<std::optional<InternalRecord>> candidate_result =
            it->table->find_latest_by_key(key);

        if (!candidate_result.is_ok())
            return Result<std::optional<InternalRecord>>::fail(std::move(candidate_result.status));

        std::optional<InternalRecord> candidate = std::move(candidate_result.value);

        if (candidate.has_value() &&
            (!latest.value.has_value() || candidate->seq_num > latest.value->seq_num))
        {
            latest.value = std::move(candidate);
        }
    }

    if (!latest.value.has_value())
    {
        return Result<std::optional<InternalRecord>>::fail(Status{
            StatusCode::NotFound,
            "Key was not found in the MemTable"
            });
    }

    return Result<std::optional<InternalRecord>>::ok(std::move(*(latest.value)));
}

Status MemTable::freeze_mutable()
{
    std::shared_ptr<RBTree> replacement;

    try
    {
        // Allocate before taking the lock. If allocation fails, the active
        // table and immutable queue are unchanged.
        replacement = std::make_shared<RBTree>();
    }
    catch (const std::bad_alloc&)
    {
        return Status{
            StatusCode::OutOfMemory,
            "Failed to allocate a replacement mutable table"
        };
    }

    //std::unique_lock lock(mutex_);

    if (mutable_table_->empty())
        return Status::ok();

    try
    {
        immutable_tables_.push_back(ImmutableTable{
            next_generation_id_,
            std::shared_ptr<const RBTree>(mutable_table_)
            });
    }
    catch (const std::bad_alloc&)
    {
        return Status{
            StatusCode::OutOfMemory,
            "Failed to append the frozen table to the immutable queue"
        };
    }

    ++next_generation_id_;
    mutable_table_ = std::move(replacement);
    return Status::ok();
}

Status MemTable::manual_freeze()
{
    return freeze_mutable();
}

Result<MemTable::ImmutableSnapshot> MemTable::oldest_immutable() const
{
    //std::shared_lock lock(mutex_);

    if (immutable_tables_.empty())
    {
        return Result<ImmutableSnapshot>::fail(Status{
            StatusCode::NotFound,
            "No immutable MemTable is available for flushing"
            });
    }

    const ImmutableTable& oldest = immutable_tables_.front();
    return Result<ImmutableSnapshot>::ok(ImmutableSnapshot{
        oldest.generation_id,
        oldest.table
        });
}

bool MemTable::retire_oldest_immutable(
    std::uint64_t generation_id
)
{
    //std::unique_lock lock(mutex_);

    if (immutable_tables_.empty() ||
        immutable_tables_.front().generation_id != generation_id)
    {
        return false;
    }

    immutable_tables_.pop_front();
    return true;
}

Status MemTable::dump_oldest_immutable(
    std::vector<InternalRecord>& out,
    std::uint64_t& generation_id
) const
{
    Result<ImmutableSnapshot> snapshot_result = oldest_immutable();
    if (!snapshot_result.is_ok())
        return snapshot_result.status;

    std::vector<InternalRecord> dumped;

    try
    {
        snapshot_result.value.table->dump_inorder(dumped);
    }
    catch (const std::bad_alloc&)
    {
        return Status{
            StatusCode::OutOfMemory,
            "Failed to materialize the immutable MemTable for flushing"
        };
    }

    generation_id = snapshot_result.value.generation_id;
    out = std::move(dumped);
    return Status::ok();
}

bool MemTable::has_immutable() const
{
    //std::shared_lock lock(mutex_);
    return !immutable_tables_.empty();
}

std::size_t MemTable::immutable_count() const
{
    //std::shared_lock lock(mutex_);
    return immutable_tables_.size();
}

std::size_t MemTable::mutable_memory_usage() const
{
    //std::shared_lock lock(mutex_);
    return mutable_table_->approximate_memory_usage();
}

std::size_t MemTable::approximate_memory_usage() const
{
    //std::shared_lock lock(mutex_);

    std::size_t total = mutable_table_->approximate_memory_usage();
    for (const ImmutableTable& immutable : immutable_tables_)
        total += immutable.table->approximate_memory_usage();

    return total;
}