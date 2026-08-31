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
#include "driver.h"
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
class SkipList : public Driver
{
public:
	struct Node : public ::VirtualNode
	{
		std::vector<Node*> next; // next pointers for each level
		std::uint32_t height;

		Node(ArenaEntry key, ArenaEntry value, std::uint64_t seq, ::Type t, std::uint32_t height)
			: ::VirtualNode(key, value, t, seq), next(height, nullptr), height(height)
		{
		}
		
		std::size_t approximate_memory_usage() const override;
	};

private:
	/** @brief Maximum level for the skip list. 
	* @note This is a static constant and can be adjusted based on expected number of elements and performance requirements. 
	* @todo Consider making this configurable or dynamically adjustable based on the size of the skip list.
	*/
	static constexpr std::uint32_t MAX_LEVEL = 20;
	static constexpr double PROBABILITY = 0.5; // Probability for promoting a node to the next level

	Node* head{ nullptr };
	std::uint32_t current_level = 1;

	std::mt19937 rng{ std::random_device{}() };
	std::bernoulli_distribution promote{ PROBABILITY };

	std::uint32_t random_height();

	void destroy();

	void inorder_traverse(std::vector<const Node*>& collect) const;

	/** @brief Dumps all records at a specific level in internal-key order. */
	void dump_level_inorder(std::vector<InternalRecord>& out, int level) const;

public:
	SkipList();
	~SkipList() override;

	/** @brief Non-owning iterator over records in internal-key order. */
	class InorderIterator : public ::VirtualInorderIterator
	{
	private:
		Node* current;
	
	public:
	    InorderIterator(Node* head);
	
	    bool has_next() override;
	    Node* next() override;
	};

	/**
	* @brief Inserts one versioned record and restores red-black invariants.
	* @callgraph
	*/
	::Status insert(const InternalRecord& entry) override;

	/** @brief Returns the highest-sequence record for @p key, if present. */
	Result<std::optional<InternalRecord>> find_latest_by_key(ArenaEntry key) override;
	Result<std::optional<InternalRecord>> find_latest_by_key(ArenaEntry key) const override;

	bool validate() const override;
	std::size_t approximate_memory_usage() const override;
    bool empty() const noexcept override;

	/** @brief Appends all records to @p out in internal-key order. */
	void dump_inorder(std::vector<InternalRecord>& out) const override;

	friend class MemTable;
};
