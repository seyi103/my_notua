import 'dart:async';

import '../state/playlist_models.dart';

enum SyncStage { photos, order, interval, disconnect }

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
    final stages = <SyncStage>[
      if (plan.uploads.isNotEmpty) SyncStage.photos,
      if (plan.orderChanged || plan.deletedIds.isNotEmpty) SyncStage.order,
      if (plan.intervalChanged) SyncStage.interval,
      SyncStage.disconnect,
    ];
    for (var index = 0; index < stages.length; index++) {
      await Future<void>.delayed(delay);
      yield SyncUpdate(
        stage: stages[index],
        progress: (index + 1) / stages.length,
      );
      if (shouldFail && index == 0) {
        throw const SyncException('액자와 연결이 끊어졌어요.');
      }
    }
  }
}

class SyncException implements Exception {
  const SyncException(this.message);
  final String message;
  @override
  String toString() => message;
}
