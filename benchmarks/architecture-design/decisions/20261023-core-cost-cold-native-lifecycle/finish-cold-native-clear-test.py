from pathlib import Path
p=Path('/workspace/zyx/tests/src/storage/relocatable/property/test_HybridPropertyColdNativeLifecycle.cpp');s=p.read_text()
s=s.replace('''    void executeLogical(const LogicalGraphMutationBatch &batch) {
        auto callerMemory''','''    bool executeLogical(const LogicalGraphMutationBatch &batch) {
        bool committed = false;
        auto callerMemory''',1)
s=s.replace('''        ASSERT_TRUE(executor.execute(batch).committed());
        }
        EXPECT_EQ(callerMemory->used(), 0U);
    }''','''        committed = executor.execute(batch).committed();
        }
        EXPECT_EQ(callerMemory->used(), 0U); return committed;
    }''',1)
s=s.replace('''    executeLogical(batch);''','''    ASSERT_TRUE(executeLogical(batch));''')
s=s.replace('''        // A newly created durable pool starts at epoch1. The read-only routing
        // fixture defaults to99 to separate physical and semantic generations,
        // but99 is not a reachable retirement epoch before metadata generation99.''','''        // Lifecycle fixtures obey the existing Catalog/Retire Queue generation
        // contract. Read-only routing fixtures keep epoch99; durable fixtures use
        // initial epoch1 while retaining distinct semantic/block generations.''',1)
a=s.index('        graph::utils::OwnedAllocationScope callerScope');b=s.index('        }\n        EXPECT_EQ(callerMemory',a);part=s[a:b];s=s[:a]+''.join('    '+line if line.strip() else line for line in part.splitlines(keepends=True))+s[b:]
at=s.rfind('#endif');s=s[:at]+'''TEST_F(HybridPropertyColdNativeLifecycleTest, ActualPropertyClearKeepsLiveOwnerAndRemovesAllItsHotAndColdFields) {
    installDurableFixture(true); auto old = metadata_->openSnapshot();
    LogicalGraphMutationBatch batch;
    batch.property_replacements.push_back({{LogicalGraphMutationEntityKind::LGMK_NODE,
        LogicalGraphMutationReferenceKind::LGRK_PERSISTED, 1}, {}});
    ASSERT_TRUE(executeLogical(batch)); auto memory = std::make_shared<MaterializedByteBudget>(1U << 20);
    {
        graph::utils::OwnedAllocationScope scope(memory); auto current = metadata_->openSnapshot();
        EXPECT_NO_THROW((void)entities_->readNode(current, 1)); EXPECT_TRUE(properties_->isHybridPropertyOwner(current, {NODE, 1}));
        EXPECT_FALSE(metadata_->findEntityPropertyLocationOptional(current, {NODE, 1}));
        HybridPropertyReadContext context(io_, *slotted_pages_, *metadata_, current);
        EXPECT_EQ(context.count({NODE, 1}), std::optional<size_t>(0));
        const auto report = HybridPropertyDirectoryIntegrity::verify(context, current, memory, values_.get());
        EXPECT_EQ(report.groups, 1U); EXPECT_EQ(report.columns, 0U); EXPECT_EQ(report.memberships, 48U);
        EXPECT_EQ(HybridPropertyValueReader::read(context, current, {NODE, 48}, 1, values_.get()).value->value, graph::PropertyValue(int64_t(147)));
        HybridPropertyReadContext previous(io_, *slotted_pages_, *metadata_, old);
        const auto oldLarge = HybridPropertyValueReader::read(previous, old, {NODE, 1}, 4, values_.get()); ASSERT_TRUE(oldLarge.value);
        EXPECT_EQ(oldLarge.value->value, graph::PropertyValue(std::string(8193, 'x')));
    }
    EXPECT_NO_THROW((void)RelocatableIntegrityScanner(io_, *metadata_, memory).scan()); EXPECT_EQ(memory->used(), 0U);
}
'''+s[at:];p.write_text(s)
