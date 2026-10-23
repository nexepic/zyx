from pathlib import Path
root=Path('/workspace/zyx')
p=root/'include/graph/storage/relocatable/property/HybridPropertyReadContext.hpp';s=p.read_text();pos=s.index('    HybridPropertyReadContext(std::shared_ptr<StorageIO>')
s=s[:pos]+'''    // A cursor lends its existing ROW representation to this synchronous read.
    // No callback or bytes escape the lease; nested calls restore their parent.
    class ColdRowRepresentationLease final {
        const HybridPropertyReadContext &context_;
        const std::function<void()> *previous_;
    public:
        ColdRowRepresentationLease(const HybridPropertyReadContext &context, const std::function<void()> *release)
            : context_(context), previous_(context.before_cold_decode_) { context.before_cold_decode_ = release; }
        ~ColdRowRepresentationLease() { context_.before_cold_decode_ = previous_; }
        ColdRowRepresentationLease(const ColdRowRepresentationLease &) = delete;
        ColdRowRepresentationLease &operator=(const ColdRowRepresentationLease &) = delete;
    };
    [[nodiscard]] ColdRowRepresentationLease borrowColdRowRepresentation(const std::function<void()> *release) const {
        return ColdRowRepresentationLease(*this, release);
    }
'''+s[pos:]
s=s.replace('        const auto page = fields_.read(snapshot_, reference.page_id);\n        const auto block = page.view().readColdRows', '        if (before_cold_decode_) (*before_cold_decode_)();\n        const auto page = fields_.read(snapshot_, reference.page_id);\n        const auto block = page.view().readColdRows',1)
s=s.replace('    mutable size_t full_entity_columns_ = 8U;','    mutable size_t full_entity_columns_ = 8U;\n    mutable const std::function<void()> *before_cold_decode_ = nullptr;')
p.write_text(s)
p=root/'include/graph/storage/relocatable/property/PropertyCollectionRootStore.hpp';s=p.read_text();needle='            const PropertyColumnBatchReadSinks *batches = nullptr) const;';assert s.count(needle)==1;s=s.replace(needle,'            const PropertyColumnBatchReadSinks *batches = nullptr,\n            const std::function<void()> *beforeColdRead = nullptr) const;',1);p.write_text(s)
p=root/'src/storage/relocatable/property/PropertyCollectionRootStore.cpp';s=p.read_text();a=s.index('    bool PropertyCollectionRootStore::readHybridSelectedBatch(');b=s.index('\n    }',a);part=s[a:b]
part=part.replace('const PropertyColumnBatchReadSinks *batches) const {','const PropertyColumnBatchReadSinks *batches, const std::function<void()> *beforeColdRead) const {')
part=part.replace('                return HybridPropertyValueReader::readSelected(', '                const auto representation = context.borrowColdRowRepresentation(beforeColdRead);\n                return HybridPropertyValueReader::readSelected(',1);s=s[:a]+part+s[b:]
# Full-entity cursor: release the optional old ROW image only when cold bytes
# actually need it. Ordinary ROW locality and hot-column-only reads retain it.
a=s.index('PropertyCollectionRootScanCursor::readEntityOptional(');b=s.index('\n\tstd::optional<PropertyCollectionRootEntry>',a);part=s[a:b]
part=part.replace('                const auto pointRead = [&] {','                const auto pointRead = [&] {\n                    current_page_.reset(); current_page_id_ = PageId{};',1)
part=part.replace('                auto result = HybridPropertyValueReader::readCollection(','''                const auto releaseRow = [&] { current_page_.reset(); current_page_id_ = PageId{}; };
                const std::function<void()> beforeCold = std::ref(releaseRow);
                const auto representation = hybrid_context_->reader.borrowColdRowRepresentation(&beforeCold);
                auto result = HybridPropertyValueReader::readCollection(''',1);s=s[:a]+part+s[b:]
# Point cursor calls already delegate to stack-local point readers, never to
# their retained ROW image. Drop that unused representation before delegation.
for method in ['readEntityPropertyOptional','readEntityPropertyExists']:
 a=s.index('PropertyCollectionRootScanCursor::'+method+'(');b=s.index('\n\t}',a)+4;part=s[a:b];part=part.replace('                if (cancellation_) cancellation_->check();','                current_page_.reset(); current_page_id_ = PageId{};\n                if (cancellation_) cancellation_->check();',1);s=s[:a]+part+s[b:]
# All three batch cursor entries lend the same optional ROW image. Store-only
# callers have no retained ROW image and keep the default null callback.
for method in ['readEntityPropertiesOptionalSorted','readEntityPropertiesAndCountsOptionalSorted','readEntityPropertyCountsSorted']:
 a=s.index('PropertyCollectionRootScanCursor::'+method+'(');b=s.index('\n\t}',a)+4;part=s[a:b]
 needle='if (store_->readHybridSelectedBatch('
 pos=part.index(needle);lineend=part.index('\n',pos);line=part[pos:lineend]
 if method=='readEntityPropertiesOptionalSorted':line=line.replace('scalars, texts, batches))','scalars, texts, batches, &beforeCold))')
 elif method=='readEntityPropertiesAndCountsOptionalSorted':line=line.replace('cancellation_, admission))','cancellation_, admission, nullptr, nullptr, nullptr, nullptr, &beforeCold))')
 else:line=line.replace('results, cancellation_))','results, cancellation_, nullptr, nullptr, nullptr, nullptr, nullptr, &beforeCold))')
 part=part[:pos]+'''const auto releaseRow = [&] { current_page_.reset(); current_page_id_ = PageId{}; };
        const std::function<void()> beforeCold = std::ref(releaseRow);
        '''+line+part[lineend:];s=s[:a]+part+s[b:]
p.write_text(s)
