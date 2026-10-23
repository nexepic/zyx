from pathlib import Path
r=Path('/workspace/zyx');p=r/'tests/include/storage/relocatable/property/HybridPropertyColdRowRoutingTestFixture.hpp';s=p.read_text();s=s.replace('unsigned live_owner_count_ = 64;','unsigned live_owner_count_ = 64;\n    Generation physical_generation_{99};',1).replace('Generation(99)', 'physical_generation_').replace('io_->writeAt({offset, 99}', 'io_->writeAt({offset, physical_generation_.value()}');p.write_text(s)
p=r/'tests/src/storage/relocatable/property/test_HybridPropertyColdNativeLifecycle.cpp';s=p.read_text();s=s.replace('''        RecordProperty("fixture_stage", "seed");
        seed(large);''','''        RecordProperty("fixture_stage", "seed");
        // A newly created durable pool starts at epoch1. The read-only routing
        // fixture defaults to99 to separate physical and semantic generations,
        // but99 is not a reachable retirement epoch before metadata generation99.
        physical_generation_ = Generation(1);
        seed(large);''',1)
s=s.replace('''    void executeLogical(const LogicalGraphMutationBatch &batch) {
        // Match''','''    void executeLogical(const LogicalGraphMutationBatch &batch) {
        auto callerMemory = std::make_shared<MaterializedByteBudget>(1U << 20);
        {
        graph::utils::OwnedAllocationScope callerScope(callerMemory);
        // Match''',1)
s=s.replace('''        ASSERT_TRUE(executor.execute(batch).committed());
    }
    void verifySource''','''        ASSERT_TRUE(executor.execute(batch).committed());
        }
        EXPECT_EQ(callerMemory->used(), 0U);
    }
    void verifySource''',1);p.write_text(s)
