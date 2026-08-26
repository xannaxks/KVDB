/**
 * @file skip_list.h
 * @brief Ordered MemTable backend keyed by user key and sequence number.
 */
#pragma once

#include <variant>
#include <vector>
#include <string>
#include <cstdint>
#include "status.h"
#include <stack>
#include <functional>
#include "type.h"
#include "record.h"
#include <random>
#include "arena.h"

class MemTable;

/**
 * @brief Skip list containing versioned InternalRecord values.
 *
 * Nodes are ordered first by user key and then by descending sequence number,
 * allowing the first match for a key to represent its newest version. Standard
 * skip list search and insertion algorithms keep lookup and insertion expected and average equal to logarithmic.
 *
 * @note Nodes reference ArenaEntry storage but do not own those bytes.
 * 
 * @todo Implement abstract class for underlying MemTable driver, allowing for pluggable backends (e.g., RBTree, SkipList, etc.).
 *		 All the drivers/backend should implement the same interface, so that MemTable can use any of them interchangeably.
 */
class SkipList
{
public:
	struct Node
	{
		ArenaEntry key_entry;
		ArenaEntry value_entry;
		const std::uint64_t seq_number;
		::Type type;
		std::vector<std::unique_ptr<Node>> forward; // Forward pointers for each level

		Node(ArenaEntry key, ArenaEntry value, std::uint64_t seq, ::Type t)
			: key_entry(key), value_entry(value), seq_number(seq), type(t), forward(MAX_LEVEL, nullptr)
		{
		}

		bool operator<(const Node& other) const;
		bool operator>(const Node& other) const;
		bool operator==(const Node& other) const;
		
		size_t approximate_memory_usage() const;
	};

private:
	/** @brief Maximum level for the skip list. 
	* @note This is a static constant and can be adjusted based on expected number of elements and performance requirements. 
	* @todo Consider making this configurable or dynamically adjustable based on the size of the skip list.
	*/
	static constexpr std::uint32_t MAX_LEVEL = 20;
	static constexpr double PROBABILITY = 0.5; // Probability for promoting a node to the next level

	std::unique_ptr<Node> head{ nullptr };
	std::uint32_t current_level = 1;

	std::mt19937 rng{ std::random_device{}() };
	std::bernoulli_distribution promote{ PROBABILITY };

	std::uint32_t random_height();

	void destroy();

	template<typename Collection>
	void inorder_traverse(Collection& collect) const
	{
	    std::function<void(const Node*)> traverse = [&](const Node* current)
	        {
	        };
	
	    traverse(root);
	}

	/** @brief Dumps all records at a specific level in internal-key order. */
	void dump_level_inorder(std::vector<InternalRecord>& out, int level) const;

public:
	SkipList();
	~SkipList();

	SkipList(const SkipList&) = delete;
	SkipList& operator=(const SkipList&) = delete;

	/** @brief Non-owning iterator over records in internal-key order. */
	class InorderIterator
	{
	private:
	    //std::stack<Node*> st;
	
	    //void push_left(Node* node);
	
	public:
	    InorderIterator(Node* root);
	
	    bool has_next();
	    Node* next();
	};

	/**
	* @brief Inserts one versioned record and restores red-black invariants.
	* @callgraph
	*/
	::Status insert(const InternalRecord& entry);

	/** @brief Returns the highest-sequence record for @p key, if present. */
	Result<std::optional<InternalRecord>> find_latest_by_key(ArenaEntry key) const;

	bool validate() const;
    size_t approximate_level_memory_usage(Node* node) const;
	size_t approximate_memory_usage() const;
    bool empty() const noexcept;

	/** @brief Appends all records to @p out in internal-key order. */
	void dump_inorder(std::vector<InternalRecord>& out) const;

	friend class MemTable;
};
