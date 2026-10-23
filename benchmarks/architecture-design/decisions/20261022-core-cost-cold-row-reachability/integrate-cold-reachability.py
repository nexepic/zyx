from pathlib import Path
p=Path('include/graph/storage/relocatable/maintenance/LargeValueReachabilityReclaimer.hpp');s=p.read_text();s=s.replace('namespace graph::storage {','namespace graph::storage {\n    class HybridPropertyReadContext;\n    class EntityPropertyLargeValueOwnershipScannerTestAccess;',1);a=s.index('\tclass EntityPropertyLargeValueOwnershipScanner final');idx=s.index('\tprivate:',a);s=s[:idx]+s[idx:].replace('\tprivate:','''\tprivate:
        friend class EntityPropertyLargeValueOwnershipScannerTestAccess;
        [[nodiscard]] Progress scanReachableAfterImpl(const MetadataSnapshotPin &snapshot, uint64_t afterLocationId,
            size_t maximumLocations, size_t maximumMetadataPages, LargeValueOwnershipScanSink &sink,
            const HybridPropertyReadContext *contextOverride) const;''',1);p.write_text(s)
p=Path('src/storage/relocatable/maintenance/LargeValueReachabilityReclaimer.cpp');s=p.read_text();a=s.index('\tLargeValueOwnershipScanner::Progress EntityPropertyLargeValueOwnershipScanner::scanReachableAfter(');idx=s.index('        if (!metadata_',a);s=s[:idx]+'''        return scanReachableAfterImpl(snapshot, afterLocationId, maximumLocations, maximumMetadataPages, sink, nullptr);
    }
    LargeValueOwnershipScanner::Progress EntityPropertyLargeValueOwnershipScanner::scanReachableAfterImpl(
        const MetadataSnapshotPin &snapshot, uint64_t afterLocationId, size_t maximumLocations,
        size_t maximumMetadataPages, LargeValueOwnershipScanSink &sink,
        const HybridPropertyReadContext *contextOverride) const {
        if (contextOverride && !contextOverride->usesSnapshot(snapshot))
            fail(FormatError::FERR_INVALID_STATE, "ownership context belongs to another snapshot");
''' +s[idx:];a=s.index('        HybridPropertyReadContext context(property_pages_',a);s=s[:a]+s[a:].replace('        HybridPropertyReadContext context(property_pages_, metadata_, snapshot);','''        std::optional<HybridPropertyReadContext> loadedContext;
        if (!contextOverride) loadedContext.emplace(property_pages_, metadata_, snapshot);
        const auto &context = contextOverride ? *contextOverride : *loadedContext;''',1)
a=s.index('            auto node =',a);b=s.index('        } catch (const DirectoryLimit &)',a);chunk=s[a:b];chunk=chunk.replace('context.nextOwnerAfter(', 'context.nextOwnershipStepAfter(');chunk=chunk.replace('std::optional<EntityPropertyKey> owner;', 'std::optional<HybridPropertyReadContext::OwnershipStep> step;\n                using StepKind = HybridPropertyReadContext::OwnershipStepKind;');chunk=chunk.replace('if (node) owner = node;', 'if (node) step = node;');chunk=chunk.replace('if (edge && (!owner || edge->directoryId() < owner->directoryId())) owner = edge;', 'if (edge && (!step || edge->owner.directoryId() < step->owner.directoryId())) step = edge;\n                const auto owner = step ? std::optional(step->owner) : std::nullopt;')
idx=chunk.index('                const auto remaining = maximumLocations');end=chunk.index('                progress.last_scanned_location_id = owner->directoryId();',idx)
old=chunk[idx:end];chunk=chunk[:idx]+'''                if (step->kind == StepKind::HOSK_BOUNDARY) {
                    ++progress.locations_scanned;
                } else if (step->kind == StepKind::HOSK_COLD_OWNER) {
                    if (!context.visitColdReference(*owner, [&](const auto &reference) { sink.markReachable(reference); }, std::ref(admit)))
                        fail(FormatError::FERR_INVALID_STATE, "cold ownership step lost its whole-root reference");
                    ++progress.locations_scanned;
                } else {
''' +old+'''                }
''' +chunk[end:];s=s[:a]+chunk+s[b:];p.write_text(s)
