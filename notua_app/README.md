# Notua app

The production entry point is a local-first slideshow draft editor. The flow is
**slideshow home → photo editor → apply progress/result**. The UI is deliberately
separated from device transport through `SynchronizationService`; this phase uses
`FakeSynchronizationService` only.

`PlaylistDraft` owns selection, ordering, the five-photo limit, interval, and the
comparison with `DevicePlaylistSnapshot`. `SyncPlan` keeps image uploads, deletion,
order, and interval changes separate so an order-only edit never uploads a photo.
Every plan also carries the complete target slide ID order and target interval;
the transport layer therefore does not need to read mutable presentation state.
A failed synchronization does not commit the snapshot and therefore preserves all
pending draft changes.

In debug builds, the validated hardware screen remains available at
`/developer/transfer`. It is not registered in release builds and its Android
MethodChannel/EventChannel implementation is unchanged.
