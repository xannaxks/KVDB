#include "skip_list.h"
#include <format>
#include <vector>

// Consider moving adding init function to catch bad_alloc exception and return Status::BadAlloc instead of throwing exception.
SkipList::SkipList()
{
	std::unique_ptr<Node> head_owner = std::make_unique<Node>(ArenaEntry(nullptr, 0), ArenaEntry(nullptr, 0), 0, Type::Undefined, MAX_LEVEL);
	head = head_owner.get();
	head_owner.release();
}

::Status SkipList::insert(const InternalRecord& entry)
{
	std::vector<Node*> update;

	try {
		update.resize(MAX_LEVEL, nullptr);
	}
	catch (const std::bad_alloc&)
	{
		return ::Status{StatusCode::BadAlloc, "Failed to allocate memory for update vector"};
	}

	Node* current = head;

	for (std::int32_t level = this->current_level - 1; level >= 0; level--)
	{
		while (current->next[level] && (*(current->next[level]) < entry))
			current = current->next[level];
		update[level] = current;
	}

	current = current->next[0];

	if (current && (!(*current > entry) && !(*current < entry)))
		return ::Status{StatusCode::Duplicate, std::format("Duplicate entry for key: {}", entry.key_entry) };

	std::uint32_t height = this->random_height();

	if (height > this->current_level)
	{
		for (std::uint32_t level = this->current_level; level < height; level++)
			update[level] = head;
	}
	try
	{
		Node* new_node = nullptr;
		std::unique_ptr<Node> new_node_owner =
			std::make_unique<Node>(
				entry.key_entry,
				entry.value_entry,
				entry.seq_num,
				entry.type,
				height
			); // used smart pointer to manage memory and avoid leaks. memory gets released when exception is thrown. if no exception is thrown, we release the pointer and use it as raw pointer.
		new_node = new_node_owner.get();

		for(std::uint32_t level = 0; level < height; level++)
		{
			new_node->next[level] = update[level]->next[level];
			update[level]->next[level] = new_node;
		}

		if (height > current_level)
			current_level = height;

		new_node_owner.release();
		
		return ::Status::ok();
	}catch(const std::bad_alloc& )
	{
		return ::Status{StatusCode::BadAlloc, "Failed to allocate memory for new node"};
	}catch(...)
	{
		return ::Status{StatusCode::InsertionFailed, "An unknown error occurred"};
	}

}

Result<std::optional<InternalRecord>> SkipList::find_latest_by_key(ArenaEntry key) const
{
	Node* current = head;

	for (std::int32_t level = current_level - 1; level >= 0; level--)
	{
		while (current->next[level] && current->next[level]->key_entry < key)
			current = current->next[level];
	}

	current = current->next[0];

	if (!current || current->key_entry != key)
		return Result<std::optional<InternalRecord>>::ok(std::nullopt);

	return Result<std::optional<InternalRecord>>::ok(InternalRecord(current->key_entry, current->value_entry, current->type, current->seq_number));
}

void SkipList::inorder_traverse(std::vector<const Node*>& collect) const
{
	for (std::int32_t level = current_level - 1; level >= 0; level--)
	{
		Node* current = head->next[level];
		while (current)
		{
			collect.emplace_back(current);
			current = current->next[level];
		}
	}
}

void SkipList::destroy()
{
	Node* current = head;
	while (current)
	{
		Node* next = current->next[0];
		delete current;
		current = next;
	}
	head = nullptr;
}

SkipList::~SkipList()
{
	destroy();
}

std::uint32_t SkipList::random_height()
{
	std::uint32_t height = 1;
	while (height < MAX_LEVEL && promote(rng))
	{
		height++;
	}
	return height;
}

std::size_t SkipList::Node::approximate_memory_usage() const
{
	// Memory usage includes the size of the node itself and the size of the key and value entries, plus don't forget about heap allocation behind `next` vector.
	/*
	 ArenaEntry key_entry;
	ArenaEntry value_entry;
	const std::uint64_t seq_number;
	::Type type;
	std::vector<Node*> next;
	std::uint32_t height;*/

	return
		key_entry.approximate_memory_usage() + // ArenaEntry key_entry memory usage
		value_entry.approximate_memory_usage() + // ArenaEntry value_entry memory usage
		sizeof(std::uint64_t) + // seq_number memory usage
		sizeof(::Type) + // type memory usage
		sizeof(std::vector<Node*>) + // next vector memory usage (size of vector object
		sizeof(Node*) * next.capacity() + // plus the size of the allocated array for the vector
		sizeof(std::uint32_t); // height memory usage

}

void SkipList::dump_level_inorder(std::vector<InternalRecord>& out, int level) const
{
	if (level < 0 || level >= MAX_LEVEL)
		return;
	Node* current = head->next[level];
	while (current)
	{
		out.emplace_back(current->key_entry, current->value_entry, current->type, current->seq_number);
		current = current->next[level];
	}
}

bool SkipList::validate() const
{
	Node* current = head;
	while (current)
	{
		for (std::uint32_t level = 0; level < current->height; ++level)
		{
			if (current->next[level] && (*(current->next[level]) < *current))
				return false;
		}
		current = current->next[0];
	}
	return true; 
}

bool SkipList::empty() const noexcept
{
	return head->next[0] == nullptr;
}

void SkipList::dump_inorder(std::vector<InternalRecord>& out) const
{
	this->dump_level_inorder(out, 0);
}

std::size_t SkipList::approximate_memory_usage() const
{
	std::size_t total_size = 0;
	Node* current = head;

	while (current)
	{
		total_size += current->approximate_memory_usage();
		current = current->next[0];
	}
	return total_size;
}

SkipList::SkipList::InorderIterator::InorderIterator(Node* head)
{
	current = head->next[0]; // Start from the first element at level 0
}

bool SkipList::InorderIterator::has_next()
{
	return current != nullptr;
}

SkipList::Node* SkipList::InorderIterator::next()
{
	if (!current)
		return nullptr;
	Node* result = current;
	current = current->next[0]; // Move to the next node at level 0
	return result;
}