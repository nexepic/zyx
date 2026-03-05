# Storage System

## Segment-Based Architecture

Metrix uses a custom file format with fixed-size segments for efficient space management.

## Components

- **FileHeaderManager**: Manages file-level metadata
- **SegmentTracker**: Tracks segment allocation
- **DataManager**: Handles node/edge/property data
- **IndexManager**: Manages label and property indexes
- **DeletionManager**: Tombstone management
- **CacheManager**: LRU cache with dirty tracking

## WAL Integration

All modifications are logged to the Write-Ahead Log before actual disk writes.
