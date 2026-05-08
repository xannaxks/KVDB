#include <gtest/gtest.h>
#include "arena.h"
#include "red_black_tree.h"
#include <map>

TEST(RBTreeTest, EmptyTree)
{
	RBTree tree;
	Arena arena;

	EXPECT_TRUE(tree.validate());

	auto key = ArenaEntry::ArenaEntry::make_entry(arena, "mock");
	auto res = tree.find_latest_by_key(key);

	ASSERT_TRUE(std::holds_alternative<RBTree::Status>(res));
	EXPECT_EQ(std::get<RBTree::Status>(res), RBTree::Status::KeyNotFound);

	std::vector<InternalRecord> records;
	tree.dump_inorder(records);

	EXPECT_TRUE(records.empty());

	EXPECT_EQ(tree.approximate_memory_usage(), 0);
}

TEST(RBTreeTests, SingleEntry)
{
	Arena arena;
	RBTree tree;

	auto key = ArenaEntry::make_entry(arena, "key");
	auto value = ArenaEntry::make_entry(arena, "value");

	InternalRecord record(key, value, Type::Put, 1u);

	EXPECT_EQ(tree.insert(record), RBTree::Status::OK);
	EXPECT_TRUE(tree.validate());

	auto res = tree.find_latest_by_key(key);
	ASSERT_TRUE(std::holds_alternative<InternalRecord>(res));

	auto found_record = std::get<InternalRecord>(res);
	EXPECT_EQ(found_record, record);
}

TEST(RBTreeTest, OrderingWithManyEntries)
{
	Arena arena;
	RBTree tree;

	auto key1 = ArenaEntry::make_entry(arena, "key1");
	auto value1 = ArenaEntry::make_entry(arena, "value1");
	InternalRecord record1(key1, value1, Type::Put, 1u);

	auto key2 = ArenaEntry::make_entry(arena, "key2");
	auto value2 = ArenaEntry::make_entry(arena, "value2");
	InternalRecord record2(key2, value2, Type::Put, 2u);

	auto key3 = ArenaEntry::make_entry(arena, "key3");
	auto value3 = ArenaEntry::make_entry(arena, "value3");
	InternalRecord record3(key3, value3, Type::Put, 3u);

	auto key4 = ArenaEntry::make_entry(arena, "key4");
	auto value4 = ArenaEntry::make_entry(arena, "value4");
	InternalRecord record4(key4, value4, Type::Put, 4u);

	EXPECT_EQ(tree.insert(record2), RBTree::Status::OK);
	EXPECT_EQ(tree.insert(record1), RBTree::Status::OK);
	EXPECT_EQ(tree.insert(record4), RBTree::Status::OK);
	EXPECT_EQ(tree.insert(record3), RBTree::Status::OK);

	EXPECT_TRUE(tree.validate());

	std::vector<InternalRecord> records;

	tree.dump_inorder(records);

	EXPECT_EQ(records.size(), 4);
	EXPECT_EQ(records[0], record1);
	EXPECT_EQ(records[1], record2);
	EXPECT_EQ(records[2], record3);
	EXPECT_EQ(records[3], record4);

	auto res = tree.find_latest_by_key(key1);
	ASSERT_TRUE(std::holds_alternative<InternalRecord>(res));
	EXPECT_EQ(std::get<InternalRecord>(res), record1);

	res = tree.find_latest_by_key(key2);
	ASSERT_TRUE(std::holds_alternative<InternalRecord>(res));
	EXPECT_EQ(std::get<InternalRecord>(res), record2);

	res = tree.find_latest_by_key(key3);
	ASSERT_TRUE(std::holds_alternative<InternalRecord>(res));
	EXPECT_EQ(std::get<InternalRecord>(res), record3);

	res = tree.find_latest_by_key(key4);
	ASSERT_TRUE(std::holds_alternative<InternalRecord>(res));
	EXPECT_EQ(std::get<InternalRecord>(res), record4);
}

TEST(RBTreeTest, LatestSequenceWins)
{
	Arena arena;
	RBTree tree;

	auto key = ArenaEntry::make_entry(arena, "key");
	auto value = ArenaEntry::make_entry(arena, "value");
	InternalRecord record1(key, value, Type::Put, 1u);
	InternalRecord record2(key, value, Type::Put, 2u); // is it okay that its two same entries? even memory-wise?

	EXPECT_EQ(tree.insert(record1), RBTree::Status::OK);
	EXPECT_EQ(tree.insert(record2), RBTree::Status::OK);

	EXPECT_TRUE(tree.validate());

	auto res = tree.find_latest_by_key(key);
	ASSERT_TRUE(std::holds_alternative<InternalRecord>(res));
	EXPECT_EQ(std::get<InternalRecord>(res), record2);
}

TEST(RBTreeTest, RejectDuplicates)
{
	// RBTree accepts everything as long as it doesn't break ordering
	Arena arena;
	RBTree tree;

	auto key = ArenaEntry::make_entry(arena, "key");
	auto value = ArenaEntry::make_entry(arena, "value");
	InternalRecord record1(key, value, Type::Put, 1u);

	EXPECT_EQ(tree.insert(record1), RBTree::Status::OK);

	EXPECT_TRUE(tree.validate());

	EXPECT_EQ(tree.insert(record1), RBTree::Status::Duplicate); // duplicate records	 with same seq_num should be rejected

	EXPECT_TRUE(tree.validate());
}

TEST(RBTreeTest, TombstoneBeatsOlderPut)
{
	Arena arena;
	RBTree tree;

	auto key = ArenaEntry::make_entry(arena, "key");
	auto value = ArenaEntry::make_entry(arena, "value");
	InternalRecord record1(key, value, Type::Put, 1u);
	InternalRecord record2(key, {}, Type::Tombstone, 2u);

	EXPECT_EQ(tree.insert(record1), RBTree::Status::OK);
	EXPECT_EQ(tree.insert(record2), RBTree::Status::OK);

	EXPECT_TRUE(tree.validate());

	auto res = tree.find_latest_by_key(key);

	ASSERT_TRUE(std::holds_alternative<InternalRecord>(res));

	auto found = std::get<InternalRecord>(res);
	EXPECT_EQ(found.type, Type::Tombstone);
	EXPECT_EQ(found.seq_num, 2u);
}

TEST(RBTreeTest, InsertedKeyOutOfSequence)
{
	// RBTree should accept entries with out-of-sequence seq_num as long as they don't break ordering
	Arena arena;
	RBTree tree;

	auto key = ArenaEntry::make_entry(arena, "key");
	auto value = ArenaEntry::make_entry(arena, "value");
	InternalRecord record1(key, value, Type::Put, 2u);
	InternalRecord record2(key, value, Type::Tombstone, 234u);
	InternalRecord record3(key, value, Type::Put, 9999u);
	InternalRecord record4(key, value, Type::Tombstone, 67676767u);

	EXPECT_EQ(tree.insert(record1), RBTree::Status::OK);
	EXPECT_EQ(tree.insert(record2), RBTree::Status::OK);
	EXPECT_EQ(tree.insert(record3), RBTree::Status::OK);
	EXPECT_EQ(tree.insert(record4), RBTree::Status::OK);

	EXPECT_TRUE(tree.validate());

	std::vector<InternalRecord> records;
	tree.dump_inorder(records);

	EXPECT_EQ(records.size(), 4);
	EXPECT_EQ(records[0], record4);
	EXPECT_EQ(records[1], record3);
	EXPECT_EQ(records[2], record2);
	EXPECT_EQ(records[3], record1);
}

TEST(RBTreeTest, InsertSameKey)
{
	// RBTree should accept entries with out-of-sequence seq_num as long as they don't break ordering
	Arena arena;
	RBTree tree;

	auto key = ArenaEntry::make_entry(arena, "key");
	auto value = ArenaEntry::make_entry(arena, "value");
	InternalRecord record1(key, value, Type::Put, 2u);
	InternalRecord record2(key, value, Type::Tombstone, 234u);
	InternalRecord record3(key, value, Type::Put, 9999u);
	InternalRecord record4(key, value, Type::Tombstone, 67676767u);

	EXPECT_EQ(tree.insert(record1), RBTree::Status::OK);
	EXPECT_EQ(tree.insert(record2), RBTree::Status::OK);
	EXPECT_EQ(tree.insert(record3), RBTree::Status::OK);
	EXPECT_EQ(tree.insert(record4), RBTree::Status::OK);

	EXPECT_TRUE(tree.validate());

	std::vector<InternalRecord> records;
	tree.dump_inorder(records);

	EXPECT_EQ(records.size(), 4);
	EXPECT_EQ(records[0], record4);
	EXPECT_EQ(records[1], record3);
	EXPECT_EQ(records[2], record2);
	EXPECT_EQ(records[3], record1);

	auto res = tree.find_latest_by_key(key);
	ASSERT_TRUE(std::holds_alternative<InternalRecord>(res));
	EXPECT_EQ(std::get<InternalRecord>(res).type, Type::Tombstone);
}

TEST(RBTreeArenaTest, ArenaLifetime)
{
	Arena arena;
	RBTree tree;

	auto key = ArenaEntry::make_entry(arena, "key");
	auto value = ArenaEntry::make_entry(arena, "value");
	InternalRecord record(key, value, Type::Put, 1u);

	EXPECT_EQ(tree.insert(record), RBTree::Status::OK);

	EXPECT_TRUE(tree.validate());

	auto res = tree.find_latest_by_key(key);
	ASSERT_TRUE(std::holds_alternative<InternalRecord>(res));
	EXPECT_EQ(std::get<InternalRecord>(res), record);

	auto res_record = std::get<InternalRecord>(res);

	EXPECT_EQ(res_record.key_entry, key);
	EXPECT_EQ(res_record.value_entry, value);

	std::string key_str(reinterpret_cast<const char*>(res_record.key_entry.data), res_record.key_entry.size);
	std::string value_str(reinterpret_cast<const char*>(res_record.value_entry.data), res_record.value_entry.size);

	EXPECT_EQ(key_str, "key");
	EXPECT_EQ(value_str, "value");
}

TEST(RBTreeArenaTest, ManyWritesArenaStability)
{
	Arena arena;
	RBTree tree;

	auto key = ArenaEntry::make_entry(arena, "stable-key");
	auto val = ArenaEntry::make_entry(arena, "stable-value");

	tree.insert({ key, val, Type::Put, 1 });

	for (int i = 0; i < 10000; i++) {
		ArenaEntry::make_entry(arena, "noise-" + std::to_string(i));
	}

	auto res = tree.find_latest_by_key(key);
	ASSERT_TRUE(std::holds_alternative<InternalRecord>(res));

	auto found = std::get<InternalRecord>(res);
	EXPECT_EQ(std::string(reinterpret_cast<const char*>(found.value_entry.data), found.value_entry.size), "stable-value");
}

TEST(RBTreeTest, TreeInvariantsAfterManyWritesAscending)
{
	Arena arena;
	RBTree tree;

	for (int i = 1; i < 10000; i++) {
		auto key = ArenaEntry::make_entry(arena, "key-" + std::to_string(i));
		auto val = ArenaEntry::make_entry(arena, "value-" + std::to_string(i));

		ASSERT_EQ(tree.insert({ key, val, Type::Put, static_cast<uint64_t>(i) }),
			RBTree::Status::OK);

		ASSERT_TRUE(tree.validate()) << "failed after insert " << i;
	}
}

TEST(RBTreeTest, TreeInvariantsAfterManyWritesDescending)
{
	Arena arena;
	RBTree tree;

	for (int i = 1000; i >= 1; i--) {
		auto key = ArenaEntry::make_entry(arena, "key-" + std::to_string(i));
		auto val = ArenaEntry::make_entry(arena, "value-" + std::to_string(i));

		ASSERT_EQ(tree.insert({ key, val, Type::Put, static_cast<uint64_t>(i) }),
			RBTree::Status::OK);

		ASSERT_TRUE(tree.validate()) << "failed after insert " << i;
	}
}

TEST(RBTreeTest, TreeRandomInsert)
{
	Arena arena;
	RBTree tree;
	std::map<std::string, std::string>mp;
	for (int i = 1; i <= 10000; i++)
	{
		int num = rand() % 1000 + 1;
		auto key = ArenaEntry::make_entry(arena, "key-" + std::to_string(num));
		auto val = ArenaEntry::make_entry(arena, "value-" + std::to_string(num));

		ASSERT_EQ(tree.insert({ key, val, Type::Put, static_cast<uint64_t>(i) }),
			RBTree::Status::OK);
		ASSERT_TRUE(tree.validate()) << "failed after insert " << num;

		mp[std::string(reinterpret_cast<const char*>(key.data), key.size)]
			= std::string(reinterpret_cast<const char*>(val.data), val.size);
	}

	for (auto& [k, v] : mp)
	{
		auto res = tree.find_latest_by_key(ArenaEntry::make_entry(arena, k));
		ASSERT_TRUE(std::holds_alternative<InternalRecord>(res));

		auto found = std::get<InternalRecord>(res);
		EXPECT_EQ(std::string(reinterpret_cast<const char*>(found.value_entry.data), found.value_entry.size), v);
	}
}

TEST(RBTreeTest, ApproximateMemoryUsage)
{
	Arena arena;
	RBTree tree;

	tree.insert({ ArenaEntry::make_entry(arena, "a"), ArenaEntry::make_entry(arena, "111"), Type::Put, 1 });
	tree.insert({ ArenaEntry::make_entry(arena, "bb"), ArenaEntry::make_entry(arena, "2222"), Type::Put, 2 });

	size_t expected =
		2 * sizeof(RBTree::Node)
		+ 1 + 3
		+ 2 + 4;

	EXPECT_GT(tree.approximate_memory_usage(), 0);

	size_t previous_usage = tree.approximate_memory_usage();

	tree.insert({ ArenaEntry::make_entry(arena, "ccc"), ArenaEntry::make_entry(arena, "33333"), Type::Put, 3 });

	EXPECT_GE(tree.approximate_memory_usage(), previous_usage);
}

TEST(RBTreeTest, UAndPAreRedBalancing)
{
	Arena arena;
	RBTree tree;

	auto g = ArenaEntry::make_entry(arena, "key5");
	tree.insert({ g, ArenaEntry::make_entry(arena, "value1"), Type::Put, 1 });
	ASSERT_TRUE(tree.validate());

	{
		RBTree::InorderIterator it(tree.root_getter());

		ASSERT_TRUE(it.has_next());
		ASSERT_EQ(*(it.next()), *tree.root_getter());
		ASSERT_TRUE(tree.root_is_black());

		ASSERT_FALSE(it.has_next());
	}

	auto u = ArenaEntry::make_entry(arena, "key6");
	tree.insert({ u, ArenaEntry::make_entry(arena, "value6"), Type::Put, 2 });
	ASSERT_TRUE(tree.validate());

	{
		RBTree::InorderIterator it(tree.root_getter());

		ASSERT_TRUE(it.has_next());
		ASSERT_EQ(*(it.next()), *tree.root_getter());
		ASSERT_TRUE(tree.root_is_black());

		ASSERT_TRUE(it.has_next());
		ASSERT_EQ(*(it.next()), *tree.root_getter()->right);
		ASSERT_EQ(tree.root_getter()->right->color, RBTree::Node::Color::Red);

		ASSERT_FALSE(it.has_next());
	}

	auto p = ArenaEntry::make_entry(arena, "key4");
	tree.insert({ p, ArenaEntry::make_entry(arena, "value4"), Type::Put, 3 });
	ASSERT_TRUE(tree.validate());

	{
		RBTree::InorderIterator it(tree.root_getter());

		ASSERT_TRUE(it.has_next());
		ASSERT_EQ(*(it.next()), *tree.root_getter()->left);
		ASSERT_EQ(tree.root_getter()->left->color, RBTree::Node::Color::Red);

		ASSERT_TRUE(it.has_next());
		ASSERT_EQ(*(it.next()), *tree.root_getter());
		ASSERT_TRUE(tree.root_is_black());
		
		ASSERT_TRUE(it.has_next());
		ASSERT_EQ(*(it.next()), *tree.root_getter()->right);
		ASSERT_EQ(tree.root_getter()->right->color, RBTree::Node::Color::Red);

		ASSERT_FALSE(it.has_next());
	}

	auto z = ArenaEntry::make_entry(arena, "key3");
	tree.insert({ z, ArenaEntry::make_entry(arena, "value3"), Type::Put, 4 });
	ASSERT_TRUE(tree.validate());

	{
		RBTree::InorderIterator it(tree.root_getter());

		ASSERT_NE(tree.root_getter()->left, nullptr);
		ASSERT_NE(tree.root_getter()->right, nullptr);

		ASSERT_TRUE(it.has_next());
		ASSERT_EQ(*(it.next()), *tree.root_getter()->left->left);
		ASSERT_EQ(tree.root_getter()->left->left->color, RBTree::Node::Color::Red);

		ASSERT_TRUE(it.has_next());
		ASSERT_EQ(*(it.next()), *tree.root_getter()->left);
		ASSERT_EQ(tree.root_getter()->left->color, RBTree::Node::Color::Black);

		ASSERT_TRUE(it.has_next());
		ASSERT_EQ(*(it.next()), *tree.root_getter());
		ASSERT_TRUE(tree.root_is_black());

		ASSERT_TRUE(it.has_next());
		ASSERT_EQ(*(it.next()), *tree.root_getter()->right);
		ASSERT_EQ(tree.root_getter()->right->color, RBTree::Node::Color::Black);

		ASSERT_FALSE(it.has_next());
	}


	RBTree::InorderIterator it(tree.root_getter());

	ASSERT_TRUE(it.has_next());

	std::vector<ArenaEntry*> keys;
	keys.push_back(&u);
	keys.push_back(&g);
	keys.push_back(&p);
	keys.push_back(&z);

	while (it.has_next())
	{
		auto& node = *(it.next());

		ASSERT_FALSE(keys.empty());

		EXPECT_EQ(node.key_entry, *(keys.back()));
		keys.pop_back();
	}

	ASSERT_TRUE(keys.empty());
	ASSERT_FALSE(it.has_next());

	ASSERT_EQ(tree.root_getter()->key_entry, g);
	ASSERT_EQ(tree.root_getter()->left->key_entry, p);
	ASSERT_EQ(tree.root_getter()->left->left->key_entry, z);
	ASSERT_EQ(tree.root_getter()->right->key_entry, u);

	ASSERT_TRUE(RBTree::expect_parent_links_valid(tree.root_getter(), nullptr));
}

TEST(RBTreeTest, LLBalancing)
{
	Arena arena;
	RBTree tree;

	auto g = ArenaEntry::make_entry(arena, "key5");
	tree.insert({ g, ArenaEntry::make_entry(arena, "value1"), Type::Put, 1 });
	ASSERT_TRUE(tree.validate());

	{
		RBTree::InorderIterator it(tree.root_getter());

		ASSERT_TRUE(it.has_next());
		ASSERT_EQ(*(it.next()), *tree.root_getter());
		ASSERT_TRUE(tree.root_is_black());

		ASSERT_FALSE(it.has_next());
	}

	auto p = ArenaEntry::make_entry(arena, "key4");
	tree.insert({ p, ArenaEntry::make_entry(arena, "value4"), Type::Put, 2 });
	ASSERT_TRUE(tree.validate());

	{
		RBTree::InorderIterator it(tree.root_getter());

		ASSERT_TRUE(it.has_next());
		ASSERT_EQ(*(it.next()), *tree.root_getter()->left);
		ASSERT_EQ(tree.root_getter()->left->color, RBTree::Node::Color::Red);

		ASSERT_TRUE(it.has_next());
		ASSERT_EQ(*(it.next()), *tree.root_getter());
		ASSERT_TRUE(tree.root_is_black());

		ASSERT_FALSE(it.has_next());
	}

	auto z = ArenaEntry::make_entry(arena, "key3");
	tree.insert({ z, ArenaEntry::make_entry(arena, "value3"), Type::Put, 3 });
	ASSERT_TRUE(tree.validate());

	{
		RBTree::InorderIterator it(tree.root_getter());

		ASSERT_NE(tree.root_getter()->left, nullptr);
		ASSERT_NE(tree.root_getter()->right, nullptr);

		ASSERT_TRUE(it.has_next());
		ASSERT_EQ(*(it.next()), *tree.root_getter()->left);
		ASSERT_EQ(tree.root_getter()->left->color, RBTree::Node::Color::Red);

		ASSERT_TRUE(it.has_next());
		ASSERT_EQ(*(it.next()), *tree.root_getter());
		ASSERT_TRUE(tree.root_is_black());
		
		ASSERT_TRUE(it.has_next());
		ASSERT_EQ(*(it.next()), *tree.root_getter()->right);
		ASSERT_EQ(tree.root_getter()->right->color, RBTree::Node::Color::Red);

		ASSERT_FALSE(it.has_next());
	}

	RBTree::InorderIterator it(tree.root_getter());

	ASSERT_TRUE(it.has_next());

	std::vector<ArenaEntry*> keys;
	keys.push_back(&g);
	keys.push_back(&p);
	keys.push_back(&z);

	while (it.has_next())
	{
		auto& node = *(it.next());

		ASSERT_FALSE(keys.empty());

		EXPECT_EQ(node.key_entry, *(keys.back()));
		keys.pop_back();
	}

	ASSERT_TRUE(keys.empty());
	ASSERT_FALSE(it.has_next());

	ASSERT_EQ(tree.root_getter()->key_entry, p);
	ASSERT_EQ(tree.root_getter()->left->key_entry, z);
	ASSERT_EQ(tree.root_getter()->right->key_entry, g);

	ASSERT_TRUE(RBTree::expect_parent_links_valid(tree.root_getter(), nullptr));
}

TEST(RBTreeTest, RRBalancing)
{
	Arena arena;
	RBTree tree;

	auto g = ArenaEntry::make_entry(arena, "key3");
	tree.insert({ g, ArenaEntry::make_entry(arena, "value1"), Type::Put, 1 });
	ASSERT_TRUE(tree.validate());

	{
		RBTree::InorderIterator it(tree.root_getter());

		ASSERT_TRUE(it.has_next());
		ASSERT_EQ(*(it.next()), *tree.root_getter());
		ASSERT_TRUE(tree.root_is_black());

		ASSERT_FALSE(it.has_next());
	}

	auto p = ArenaEntry::make_entry(arena, "key4");
	tree.insert({ p, ArenaEntry::make_entry(arena, "value4"), Type::Put, 2 });
	ASSERT_TRUE(tree.validate());

	{
		RBTree::InorderIterator it(tree.root_getter());

		ASSERT_TRUE(it.has_next());
		ASSERT_EQ(*(it.next()), *tree.root_getter());
		ASSERT_TRUE(tree.root_is_black());

		ASSERT_TRUE(it.has_next());
		ASSERT_EQ(*(it.next()), *tree.root_getter()->right);
		ASSERT_EQ(tree.root_getter()->right->color, RBTree::Node::Color::Red);

		ASSERT_FALSE(it.has_next());
	}

	auto z = ArenaEntry::make_entry(arena, "key5");
	tree.insert({ z, ArenaEntry::make_entry(arena, "value3"), Type::Put, 3 });
	ASSERT_TRUE(tree.validate());

	{
		RBTree::InorderIterator it(tree.root_getter());

		ASSERT_NE(tree.root_getter()->left, nullptr);
		ASSERT_NE(tree.root_getter()->right, nullptr);

		ASSERT_TRUE(it.has_next());
		ASSERT_EQ(*(it.next()), *tree.root_getter()->left);
		ASSERT_EQ(tree.root_getter()->left->color, RBTree::Node::Color::Red);

		ASSERT_TRUE(it.has_next());
		ASSERT_EQ(*(it.next()), *tree.root_getter());
		ASSERT_TRUE(tree.root_is_black());

		ASSERT_TRUE(it.has_next());
		ASSERT_EQ(*(it.next()), *tree.root_getter()->right);
		ASSERT_EQ(tree.root_getter()->right->color, RBTree::Node::Color::Red);

		ASSERT_FALSE(it.has_next());
	}

	RBTree::InorderIterator it(tree.root_getter());

	ASSERT_TRUE(it.has_next());

	std::vector<ArenaEntry*> keys;
	keys.push_back(&z);
	keys.push_back(&p);
	keys.push_back(&g);

	while (it.has_next())
	{
		auto& node = *(it.next());

		ASSERT_FALSE(keys.empty());

		EXPECT_EQ(node.key_entry, *(keys.back()));
		keys.pop_back();
	}

	ASSERT_TRUE(keys.empty());
	ASSERT_FALSE(it.has_next());

	ASSERT_EQ(tree.root_getter()->key_entry, p);
	ASSERT_EQ(tree.root_getter()->left->key_entry, g);
	ASSERT_EQ(tree.root_getter()->right->key_entry, z);

	ASSERT_TRUE(RBTree::expect_parent_links_valid(tree.root_getter(), nullptr));
}

TEST(RBTreeTest, LRBalancing)
{
	Arena arena;
	RBTree tree;

	auto g = ArenaEntry::make_entry(arena, "key5");
	tree.insert({ g, ArenaEntry::make_entry(arena, "value1"), Type::Put, 1 });
	ASSERT_TRUE(tree.validate());

	{
		RBTree::InorderIterator it(tree.root_getter());

		ASSERT_TRUE(it.has_next());
		ASSERT_EQ(*(it.next()), *tree.root_getter());
		ASSERT_TRUE(tree.root_is_black());

		ASSERT_FALSE(it.has_next());
	}

	auto p = ArenaEntry::make_entry(arena, "key3");
	tree.insert({ p, ArenaEntry::make_entry(arena, "value4"), Type::Put, 2 });
	ASSERT_TRUE(tree.validate());

	{
		RBTree::InorderIterator it(tree.root_getter());

		ASSERT_TRUE(it.has_next());
		ASSERT_EQ(*(it.next()), *tree.root_getter()->left);
		ASSERT_EQ(tree.root_getter()->left->color, RBTree::Node::Color::Red);

		ASSERT_TRUE(it.has_next());
		ASSERT_EQ(*(it.next()), *tree.root_getter());
		ASSERT_TRUE(tree.root_is_black());

		ASSERT_FALSE(it.has_next());
	}

	auto z = ArenaEntry::make_entry(arena, "key4");
	tree.insert({ z, ArenaEntry::make_entry(arena, "value3"), Type::Put, 3 });
	ASSERT_TRUE(tree.validate());

	{
		RBTree::InorderIterator it(tree.root_getter());

		ASSERT_NE(tree.root_getter()->left, nullptr);
		ASSERT_NE(tree.root_getter()->right, nullptr);

		ASSERT_TRUE(it.has_next());
		ASSERT_EQ(*(it.next()), *tree.root_getter()->left);
		ASSERT_EQ(tree.root_getter()->left->color, RBTree::Node::Color::Red);

		ASSERT_TRUE(it.has_next());
		ASSERT_EQ(*(it.next()), *tree.root_getter());
		ASSERT_TRUE(tree.root_is_black());

		ASSERT_TRUE(it.has_next());
		ASSERT_EQ(*(it.next()), *tree.root_getter()->right);
		ASSERT_EQ(tree.root_getter()->right->color, RBTree::Node::Color::Red);

		ASSERT_FALSE(it.has_next());
	}

	RBTree::InorderIterator it(tree.root_getter());

	ASSERT_TRUE(it.has_next());

	std::vector<ArenaEntry*> keys;
	keys.push_back(&g);
	keys.push_back(&z);
	keys.push_back(&p);

	while (it.has_next())
	{
		auto& node = *(it.next());

		ASSERT_FALSE(keys.empty());

		EXPECT_EQ(node.key_entry, *(keys.back()));
		keys.pop_back();
	}

	ASSERT_TRUE(keys.empty());
	ASSERT_FALSE(it.has_next());

	ASSERT_EQ(tree.root_getter()->key_entry, z);
	ASSERT_EQ(tree.root_getter()->left->key_entry, p);
	ASSERT_EQ(tree.root_getter()->right->key_entry, g);

	ASSERT_TRUE(RBTree::expect_parent_links_valid(tree.root_getter(), nullptr));
}

TEST(RBTreeTest, RLBalancing)
{
	Arena arena;
	RBTree tree;

	auto g = ArenaEntry::make_entry(arena, "key3");
	tree.insert({ g, ArenaEntry::make_entry(arena, "value1"), Type::Put, 1 });
	ASSERT_TRUE(tree.validate());

	{
		RBTree::InorderIterator it(tree.root_getter());

		ASSERT_TRUE(it.has_next());
		ASSERT_EQ(*(it.next()), *tree.root_getter());
		ASSERT_TRUE(tree.root_is_black());

		ASSERT_FALSE(it.has_next());
	}

	auto p = ArenaEntry::make_entry(arena, "key5");
	tree.insert({ p, ArenaEntry::make_entry(arena, "value4"), Type::Put, 2 });
	ASSERT_TRUE(tree.validate());

	{
		RBTree::InorderIterator it(tree.root_getter());

		ASSERT_TRUE(it.has_next());
		ASSERT_EQ(*(it.next()), *tree.root_getter());
		ASSERT_TRUE(tree.root_is_black());

		ASSERT_TRUE(it.has_next());
		ASSERT_EQ(*(it.next()), *tree.root_getter()->right);
		ASSERT_EQ(tree.root_getter()->right->color, RBTree::Node::Color::Red);

		ASSERT_FALSE(it.has_next());
	}

	auto z = ArenaEntry::make_entry(arena, "key4");
	tree.insert({ z, ArenaEntry::make_entry(arena, "value3"), Type::Put, 3 });
	ASSERT_TRUE(tree.validate());

	{
		RBTree::InorderIterator it(tree.root_getter());

		ASSERT_NE(tree.root_getter()->left, nullptr);
		ASSERT_NE(tree.root_getter()->right, nullptr);

		ASSERT_TRUE(it.has_next());
		ASSERT_EQ(*(it.next()), *tree.root_getter()->left);
		ASSERT_EQ(tree.root_getter()->left->color, RBTree::Node::Color::Red);

		ASSERT_TRUE(it.has_next());
		ASSERT_EQ(*(it.next()), *tree.root_getter());
		ASSERT_TRUE(tree.root_is_black());

		ASSERT_TRUE(it.has_next());
		ASSERT_EQ(*(it.next()), *tree.root_getter()->right);
		ASSERT_EQ(tree.root_getter()->right->color, RBTree::Node::Color::Red);

		ASSERT_FALSE(it.has_next());
	}

	RBTree::InorderIterator it(tree.root_getter());

	ASSERT_TRUE(it.has_next());

	std::vector<ArenaEntry*> keys;
	keys.push_back(&p);
	keys.push_back(&z);
	keys.push_back(&g);

	while (it.has_next())
	{
		auto& node = *(it.next());

		ASSERT_FALSE(keys.empty());

		EXPECT_EQ(node.key_entry, *(keys.back()));
		keys.pop_back();
	}

	ASSERT_TRUE(keys.empty());
	ASSERT_FALSE(it.has_next());

	ASSERT_EQ(tree.root_getter()->key_entry, z);
	ASSERT_EQ(tree.root_getter()->left->key_entry, g);
	ASSERT_EQ(tree.root_getter()->right->key_entry, p);

	ASSERT_TRUE(RBTree::expect_parent_links_valid(tree.root_getter(), nullptr));
}

TEST(RBTreeTest, ParentPointerCorrectness)
{
	Arena arena;
	RBTree tree;
	const std::string key_prefix = "key-";
	const std::string value_prefix = "value-";

	//std::srand(12345); // fixed seed for reproducibility

	for (int i = 1; i <= 100; i++)
	{
		auto key = ArenaEntry::make_entry(arena, ArenaEntry::generate_random_key(key_prefix));
		auto value = ArenaEntry::make_entry(arena, ArenaEntry::generate_random_value(value_prefix));

		int rand_type = std::rand() % 2;

		if (rand_type)
			tree.insert({ key, value, Type::Put, static_cast<std::uint64_t>(i) });
		else
			tree.insert({ key, value, Type::Tombstone, static_cast<std::uint64_t>(i) });

		ASSERT_TRUE(tree.validate());
	}

	ASSERT_NE(tree.root_getter(), nullptr);
	ASSERT_EQ(tree.root_getter()->parent, nullptr);

	RBTree::InorderIterator it(tree.root_getter());

	while (it.has_next())
	{
		RBTree::Node* node = it.next();
		ASSERT_NE(node, nullptr);

		if (node == tree.root_getter())
		{
			EXPECT_EQ(node->parent, nullptr);
		}
		else
		{
			ASSERT_NE(node->parent, nullptr);

			EXPECT_TRUE(
				node->parent->left == node ||
				node->parent->right == node
			);
		}
	}

	ASSERT_FALSE(it.has_next());

	ASSERT_TRUE(RBTree::expect_parent_links_valid(tree.root_getter(), nullptr));
}

TEST(RBTreeTest, IteratorCorrectness)
{
	Arena arena;
	RBTree tree;
	const std::string key_prefix = "key-";
	const std::string value_prefix = "value-";

	//std::srand(12345); // fixed seed for reproducibility

	for (int i = 1; i <= 100; i++)
	{
		auto key = ArenaEntry::make_entry(arena, ArenaEntry::generate_random_key(key_prefix));
		auto value = ArenaEntry::make_entry(arena, ArenaEntry::generate_random_value(value_prefix));

		Type type = (std::rand() % 2)
			? Type::Put
			: Type::Tombstone;

		tree.insert({ key, value, type, static_cast<std::uint64_t>(i) });

		ASSERT_TRUE(tree.validate());
	}

	ASSERT_NE(tree.root_getter(), nullptr);

	RBTree::InorderIterator it(tree.root_getter());

	std::vector<RBTree::Node*> nodes;

	while (it.has_next())
	{
		RBTree::Node* node = it.next();
		ASSERT_NE(node, nullptr);
		nodes.push_back(node);
	}

	ASSERT_FALSE(it.has_next());

	for (std::size_t i = 1; i < nodes.size(); i++)
	{
		EXPECT_FALSE(*nodes[i] < *nodes[i - 1])
			<< "Iterator returned nodes out of tree order at index " << i;
	}
}

TEST(RBTreeTest, SupportsSmallBinaryKeys)
{
	Arena arena;
	RBTree tree;

	std::string k1(std::string("a\0b", 3));      // bytes: 61 00 62
	std::string k2(std::string("a\0c", 3));      // bytes: 61 00 63
	std::string k3(std::string("\0abc", 4));     // bytes: 00 61 62 63
	std::string k4(std::string("\xFF\x00", 2));  // bytes: FF 00

	auto key1 = ArenaEntry::make_entry(arena, k1);
	auto key2 = ArenaEntry::make_entry(arena, k2);
	auto key3 = ArenaEntry::make_entry(arena, k3);
	auto key4 = ArenaEntry::make_entry(arena, k4);

	auto value = ArenaEntry::make_entry(arena, std::string("value"));

	ASSERT_EQ(tree.insert({ key1, value, Type::Put, 1 }), RBTree::Status::OK);
	ASSERT_TRUE(tree.validate());

	ASSERT_EQ(tree.insert({ key2, value, Type::Put, 2 }), RBTree::Status::OK);
	ASSERT_TRUE(tree.validate());

	ASSERT_EQ(tree.insert({ key3, value, Type::Put, 3 }), RBTree::Status::OK);
	ASSERT_TRUE(tree.validate());

	ASSERT_EQ(tree.insert({ key4, value, Type::Put, 4 }), RBTree::Status::OK);
	ASSERT_TRUE(tree.validate());

	RBTree::InorderIterator it(tree.root_getter());

	std::vector<ArenaEntry*> expected;
	expected.push_back(&key3); // "\0abc"
	expected.push_back(&key1); // "a\0b"
	expected.push_back(&key2); // "a\0c"
	expected.push_back(&key4); // "\xFF\x00"

	for (ArenaEntry* key : expected)
	{
		ASSERT_TRUE(it.has_next());

		RBTree::Node* node = it.next();
		ASSERT_NE(node, nullptr);

		EXPECT_EQ(node->key_entry, *key);
	}

	ASSERT_FALSE(it.has_next());
}

TEST(RBTreeTest, AllowsEmptyKeysAndValuesAndOrdersVersionsByDescendingSeq)
{
	Arena arena;
	RBTree tree;

	auto empty_key = ArenaEntry::make_entry(arena, std::string("", 0));
	auto empty_value = ArenaEntry::make_entry(arena, std::string("", 0));

	ASSERT_EQ(tree.insert({ empty_key, empty_value, Type::Put, 1 }), RBTree::Status::OK);
	ASSERT_TRUE(tree.validate());

	auto key = ArenaEntry::make_entry(arena, ArenaEntry::generate_random_key("key"));
	auto value = ArenaEntry::make_entry(arena, ArenaEntry::generate_random_value("value"));

	ASSERT_EQ(tree.insert({ key, value, Type::Put, 2 }), RBTree::Status::OK);
	ASSERT_TRUE(tree.validate());

	ASSERT_EQ(tree.insert({ empty_key, value, Type::Put, 3 }), RBTree::Status::OK);
	ASSERT_TRUE(tree.validate());

	ASSERT_EQ(tree.insert({ key, empty_value, Type::Put, 4 }), RBTree::Status::OK);
	ASSERT_TRUE(tree.validate());

	std::vector<InternalRecord> expected;
	expected.push_back({ empty_key, value, Type::Put, 3 });
	expected.push_back({ empty_key, empty_value, Type::Put, 1 });
	expected.push_back({ key, empty_value, Type::Put, 4 });
	expected.push_back({ key, value, Type::Put, 2 });

	RBTree::InorderIterator it(tree.root_getter());

	for (const InternalRecord& record : expected)
	{
		ASSERT_TRUE(it.has_next());

		RBTree::Node* node = it.next();
		ASSERT_NE(node, nullptr);

		EXPECT_EQ(node->key_entry, record.key_entry);
		EXPECT_EQ(node->value_entry, record.value_entry);
		EXPECT_EQ(node->type, record.type);
		EXPECT_EQ(node->seq_number, record.seq_num);
	}

	ASSERT_FALSE(it.has_next());
}

TEST(RBTreeTest, DuplicateBehavior)
{
	// RBTree accepts multiple versions of the same key.
	// Duplicate means same key + same seq_num.
	// Value and type do not affect tree identity.

	Arena arena;
	RBTree tree;

	auto key1 = ArenaEntry::make_entry(arena, "key1");
	auto value1 = ArenaEntry::make_entry(arena, "value1");

	auto key2 = ArenaEntry::make_entry(arena, "key2");
	auto value2 = ArenaEntry::make_entry(arena, "value2");

	std::uint64_t seq1 = 1;
	std::uint64_t seq2 = 2;

	ASSERT_EQ(tree.insert({ key1, value1, Type::Put, seq1 }), RBTree::Status::OK);
	ASSERT_TRUE(tree.validate());

	ASSERT_EQ(tree.insert({ key1, value1, Type::Put, seq2 }), RBTree::Status::OK);
	ASSERT_TRUE(tree.validate());

	// Same key + same seq => duplicate, even with different value.
	ASSERT_EQ(tree.insert({ key1, value2, Type::Put, seq1 }), RBTree::Status::Duplicate);
	ASSERT_EQ(tree.insert({ key1, value2, Type::Put, seq2 }), RBTree::Status::Duplicate);

	// Exact duplicate.
	ASSERT_EQ(tree.insert({ key1, value1, Type::Put, seq1 }), RBTree::Status::Duplicate);

	// Same key + same seq => duplicate, even with different type.
	ASSERT_EQ(tree.insert({ key1, value1, Type::Tombstone, seq1 }), RBTree::Status::Duplicate);

	ASSERT_EQ(tree.insert({ key2, value1, Type::Put, seq1 }), RBTree::Status::OK);
	ASSERT_TRUE(tree.validate());

	ASSERT_EQ(tree.insert({ key2, value1, Type::Put, seq2 }), RBTree::Status::OK);
	ASSERT_TRUE(tree.validate());

	ASSERT_EQ(tree.insert({ key2, value2, Type::Put, seq1 }), RBTree::Status::Duplicate);
	ASSERT_EQ(tree.insert({ key2, value2, Type::Put, seq2 }), RBTree::Status::Duplicate);
	ASSERT_EQ(tree.insert({ key2, value1, Type::Put, seq1 }), RBTree::Status::Duplicate);
	ASSERT_EQ(tree.insert({ key2, value1, Type::Tombstone, seq1 }), RBTree::Status::Duplicate);

	ASSERT_TRUE(tree.validate());
}

TEST(RBTreeStressTest, LongKeyOrValueInsertionBehavior)
{
	Arena arena;
	RBTree tree;
	const std::string key_prefix = "key-";
	const std::string value_prefix = "value-";
	const std::size_t long_length = 100000;

	//std::srand(12345); // fixed seed for reproducibility

	for (int i = 1; i <= 100; i++)
	{
		auto key = ArenaEntry::make_entry(arena, ArenaEntry::generate_random_key(key_prefix, long_length));
		auto value = ArenaEntry::make_entry(arena, ArenaEntry::generate_random_value(value_prefix, long_length));

		Type type = (std::rand() % 2)
			? Type::Put
			: Type::Tombstone;

		tree.insert({ key, value, type, static_cast<std::uint64_t>(i) });

		ASSERT_TRUE(tree.validate());
	}

	ASSERT_NE(tree.root_getter(), nullptr);

	RBTree::InorderIterator it(tree.root_getter());

	std::vector<ArenaEntry> keys;

	while (it.has_next())
	{
		RBTree::Node* node = it.next();
		ASSERT_NE(node, nullptr);

		keys.push_back(node->key_entry);
	}

	ASSERT_FALSE(it.has_next());

	for (std::size_t i = 1; i < keys.size(); i++)
	{
		EXPECT_FALSE(keys[i] < keys[i - 1])
			<< "Keys are not in sorted order at index " << i;
	}
}