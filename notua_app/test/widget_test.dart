import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:notua_app/main.dart';
import 'package:notua_app/screens/photo_editor_screen.dart';
import 'package:notua_app/screens/sync_screen.dart';
import 'package:notua_app/state/playlist_models.dart';
import 'package:notua_app/sync/fake_sync_service.dart';

class ControlledSynchronizationService implements SynchronizationService {
  final List<StreamController<SyncUpdate>> controllers = [];

  int get attemptCount => controllers.length;

  @override
  Stream<SyncUpdate> synchronize(SyncPlan plan) {
    final controller = StreamController<SyncUpdate>();
    controllers.add(controller);
    return controller.stream;
  }

  Future<void> dispose() async {
    for (final controller in controllers) {
      if (!controller.isClosed) await controller.close();
    }
  }
}

void main() {
  testWidgets('home presents polished slideshow labels', (tester) async {
    await tester.pumpWidget(const NotuaApp());
    expect(find.text('Notua'), findsOneWidget);
    expect(find.text('거실 액자 · 연결됨'), findsOneWidget);
    expect(find.text('슬라이드 전환'), findsOneWidget);
    expect(find.text('액자에 적용하기'), findsOneWidget);
  });

  testWidgets('empty state offers a photo action', (tester) async {
    await tester.pumpWidget(
      NotuaApp(initialDraft: PlaylistDraft(slides: const [])),
    );
    expect(find.text('첫 사진을 골라주세요'), findsOneWidget);
    expect(find.text('사진 추가'), findsOneWidget);
  });

  testWidgets('editor has supported adjustment controls', (tester) async {
    await tester.pumpWidget(const MaterialApp(home: PhotoEditorScreen()));
    expect(find.text('사진 편집'), findsOneWidget);
    expect(find.text('밝기'), findsOneWidget);
    expect(find.text('대비'), findsOneWidget);
    expect(find.text('채도'), findsOneWidget);
    expect(find.text('슬라이드에 추가'), findsOneWidget);
  });

  testWidgets('each editor slider changes the preview filter', (tester) async {
    await tester.pumpWidget(const MaterialApp(home: PhotoEditorScreen()));
    for (var index = 0; index < 3; index++) {
      final before = tester
          .widget<ColorFiltered>(find.byType(ColorFiltered))
          .colorFilter;
      await tester.drag(find.byType(Slider).at(index), const Offset(45, 0));
      await tester.pump();
      final after = tester
          .widget<ColorFiltered>(find.byType(ColorFiltered))
          .colorFilter;
      expect(after, isNot(equals(before)));
    }
  });

  testWidgets('order-only sync shows accurate stages', (tester) async {
    final service = ControlledSynchronizationService();
    final draft = PlaylistDraft(
      slides: const [
        SlideItem(id: 'a', color: 0xff000000),
        SlideItem(id: 'b', color: 0xff000000),
      ],
    )..reorder(0, 1);
    await tester.pumpWidget(
      MaterialApp(
        home: SyncScreen(draft: draft, service: service),
      ),
    );
    expect(find.text('사진 전송'), findsNothing);
    expect(find.text('순서 저장'), findsOneWidget);
    expect(find.text('연결 종료'), findsOneWidget);
    await tester.pumpWidget(const SizedBox());
    await service.dispose();
  });

  testWidgets('interval-only sync shows accurate stages', (tester) async {
    final service = ControlledSynchronizationService();
    final draft = PlaylistDraft(slides: const [])..setInterval(10);
    await tester.pumpWidget(
      MaterialApp(
        home: SyncScreen(draft: draft, service: service),
      ),
    );
    expect(find.text('사진 전송'), findsNothing);
    expect(find.text('순서 저장'), findsNothing);
    expect(find.text('전환 간격 저장'), findsOneWidget);
    await tester.pumpWidget(const SizedBox());
    await service.dispose();
  });

  testWidgets('sync failure can retry and then complete', (tester) async {
    final service = ControlledSynchronizationService();
    final draft = PlaylistDraft(slides: const [])..add();
    await tester.pumpWidget(
      MaterialApp(
        home: SyncScreen(draft: draft, service: service),
      ),
    );
    service.controllers.first.addError(const SyncException('실패'));
    await tester.pump();
    expect(find.text('다시 시도'), findsOneWidget);
    await tester.tap(find.text('다시 시도'));
    await tester.pump();
    expect(service.attemptCount, 2);
    service.controllers.last.add(
      const SyncUpdate(stage: SyncStage.disconnect, progress: 1),
    );
    await tester.pump();
    expect(find.text('액자에 적용했어요'), findsOneWidget);
    expect(draft.syncPlan.hasChanges, isFalse);
    await tester.pumpWidget(const SizedBox());
    await service.dispose();
  });

  testWidgets('completion remains reachable on a short enlarged screen', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(800, 360);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final service = ControlledSynchronizationService();
    final draft = PlaylistDraft(slides: const [])..setInterval(10);
    await tester.pumpWidget(
      MediaQuery(
        data: const MediaQueryData(textScaler: TextScaler.linear(2)),
        child: MaterialApp(
          home: SyncScreen(draft: draft, service: service),
        ),
      ),
    );
    service.controllers.single.add(
      const SyncUpdate(stage: SyncStage.disconnect, progress: 1),
    );
    await tester.pump();
    await tester.scrollUntilVisible(find.text('완료'), 100);
    expect(find.text('완료'), findsOneWidget);
    await tester.pumpWidget(const SizedBox());
    await service.dispose();
  });
}
