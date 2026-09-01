import 'dart:async';

import '../state/playlist_models.dart';

enum SyncStage { photos, order, disconnect }

class SyncUpdate {
  const SyncUpdate({required this.stage, required this.progress});
  final SyncStage stage;
  final double progress;
}

abstract interface class SynchronizationService {
  Stream<SyncUpdate> synchronize(SyncPlan plan);
}

class FakeSynchronizationService implements SynchronizationService {
  FakeSynchronizationService({this.shouldFail = false, this.delay = const Duration(milliseconds: 180)});
  final bool shouldFail;
  final Duration delay;

  @override
  Stream<SyncUpdate> synchronize(SyncPlan plan) async* {
    for (var i = 1; i <= 4; i++) {
      await Future<void>.delayed(delay);
      yield SyncUpdate(stage: SyncStage.photos, progress: i * .15);
    }
    if (shouldFail) throw const SyncException('액자와 연결이 끊어졌어요.');
    await Future<void>.delayed(delay);
    yield const SyncUpdate(stage: SyncStage.order, progress: .82);
    await Future<void>.delayed(delay);
    yield const SyncUpdate(stage: SyncStage.disconnect, progress: 1);
  }
}

class SyncException implements Exception {
  const SyncException(this.message);
  final String message;
  @override
  String toString() => message;
}
