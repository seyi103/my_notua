import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:notua_app/main.dart';
import 'package:notua_app/screens/home_screen.dart';
import 'package:notua_app/screens/photo_editor_screen.dart';
import 'package:notua_app/screens/sync_screen.dart';
import 'package:notua_app/state/playlist_models.dart';
import 'package:notua_app/sync/fake_sync_service.dart';

class EmptySynchronizationService implements SynchronizationService {
  @override
  Stream<SyncUpdate> synchronize(SyncPlan plan) =>
      const Stream<SyncUpdate>.empty();
}

class ImmediateSynchronizationService implements SynchronizationService {
  @override
  Stream<SyncUpdate> synchronize(SyncPlan plan) => Stream<SyncUpdate>.value(
    const SyncUpdate(stage: SyncStage.disconnect, progress: 1),
  );
}

class RetrySynchronizationService implements SynchronizationService {
  int attemptCount = 0;
  final plans = <SyncPlan>[];

  @override
  Stream<SyncUpdate> synchronize(SyncPlan plan) {
    attemptCount++;
    plans.add(plan);
    if (attemptCount == 1) {
      return Stream<SyncUpdate>.error(const SyncException('실패'));
    }
    return Stream<SyncUpdate>.value(
      const SyncUpdate(stage: SyncStage.disconnect, progress: 1),
    );
  }
}

void useTallTestSurface(WidgetTester tester) {
  tester.view.physicalSize = const Size(800, 1400);
  tester.view.devicePixelRatio = 1;

  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);
}

void main() {
  testWidgets('home presents polished slideshow labels', (tester) async {
    useTallTestSurface(tester);
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
    expect(
      tester
          .widget<FilledButton>(find.widgetWithText(FilledButton, '액자에 적용하기'))
          .onPressed,
      isNull,
    );
  });

  testWidgets('editor has supported adjustment controls', (tester) async {
    useTallTestSurface(tester);
    await tester.pumpWidget(const MaterialApp(home: PhotoEditorScreen()));
    expect(find.text('사진 편집'), findsOneWidget);
    expect(find.text('밝기'), findsOneWidget);
    expect(find.text('대비'), findsOneWidget);
    expect(find.text('채도'), findsOneWidget);
    expect(find.text('슬라이드에 추가'), findsOneWidget);
  });

  testWidgets('each editor slider updates its real processing state', (tester) async {
    useTallTestSurface(tester);
    await tester.pumpWidget(const MaterialApp(home: PhotoEditorScreen()));
    for (final key in [
      PhotoEditorScreen.brightnessSliderKey,
      PhotoEditorScreen.contrastSliderKey,
      PhotoEditorScreen.saturationSliderKey,
    ]) {
      final before = tester.widget<Slider>(find.byKey(key)).value;
      await tester.drag(find.byKey(key), const Offset(45, 0));
      await tester.pump();
      final after = tester.widget<Slider>(find.byKey(key)).value;
      expect(after, isNot(equals(before)));
    }
  });

  testWidgets('order-only sync shows accurate stages', (tester) async {
    final draft = PlaylistDraft(
      slides: const [
        SlideItem(id: 'a', color: 0xff000000),
        SlideItem(id: 'b', color: 0xff000000),
      ],
    )..reorder(0, 1);
    await tester.pumpWidget(
      MaterialApp(
        home: SyncScreen(draft: draft, service: EmptySynchronizationService()),
      ),
    );
    expect(find.text('사진 전송'), findsNothing);
    expect(find.text('순서 저장'), findsOneWidget);
    expect(find.text('연결 종료'), findsOneWidget);
  });

  testWidgets('interval-only sync shows accurate stages', (tester) async {
    final draft = PlaylistDraft(slides: [slideForWidget('a')])..setInterval(10);
    await tester.pumpWidget(
      MaterialApp(
        home: SyncScreen(draft: draft, service: EmptySynchronizationService()),
      ),
    );
    expect(find.text('사진 전송'), findsNothing);
    expect(find.text('순서 저장'), findsNothing);
    expect(find.text('전환 간격 저장'), findsOneWidget);
  });

  testWidgets('sync failure can retry and then complete', (tester) async {
    final service = RetrySynchronizationService();
    final draft = PlaylistDraft(slides: const [])..add();
    await tester.pumpWidget(
      MaterialApp(
        home: SyncScreen(draft: draft, service: service),
      ),
    );
    await tester.pump();
    expect(find.text('다시 시도'), findsOneWidget);
    expect(draft.add(), isFalse);
    await tester.binding.handlePopRoute();
    await tester.pump();
    expect(find.text('다시 시도'), findsOneWidget);
    await tester.tap(find.text('다시 시도'));
    await tester.pump();
    expect(service.attemptCount, 2);
    expect(identical(service.plans[0], service.plans[1]), isTrue);
    await tester.pump();
    expect(find.text('액자에 적용했어요'), findsOneWidget);
    expect(draft.syncPlan.hasChanges, isFalse);
  });

  testWidgets('completion remains reachable on a short enlarged screen', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(800, 360);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    final draft = PlaylistDraft(slides: [slideForWidget('a')])..setInterval(10);
    await tester.pumpWidget(
      MediaQuery(
        data: const MediaQueryData(textScaler: TextScaler.linear(2)),
        child: MaterialApp(
          home: SyncScreen(
            draft: draft,
            service: ImmediateSynchronizationService(),
          ),
        ),
      ),
    );
    await tester.pump();
    await tester.scrollUntilVisible(find.text('완료'), 100);
    expect(find.text('완료'), findsOneWidget);
  });

  testWidgets('progress at 100% followed by error remains dirty', (
    tester,
  ) async {
    final controller = StreamController<SyncUpdate>();
    final service = _ControllerService(controller);
    final draft = PlaylistDraft(slides: [slideForWidget('a')])..add();
    await tester.pumpWidget(
      MaterialApp(
        home: SyncScreen(draft: draft, service: service),
      ),
    );
    controller.add(const SyncUpdate(stage: SyncStage.disconnect, progress: 1));
    controller.addError(const SyncException('late failure'));
    await tester.pump();
    expect(find.text('다시 시도'), findsOneWidget);
    expect(draft.syncPlan.hasChanges, isTrue);
    await controller.close();
  });

  testWidgets('home rebinds when its draft changes', (tester) async {
    useTallTestSurface(tester);
    final oldDraft = PlaylistDraft(slides: [slideForWidget('old')]);
    final newDraft = PlaylistDraft(slides: [slideForWidget('new')]);
    final service = EmptySynchronizationService();
    await tester.pumpWidget(
      MaterialApp(
        home: HomeScreen(draft: oldDraft, syncService: service),
      ),
    );
    await tester.pumpWidget(
      MaterialApp(
        home: HomeScreen(draft: newDraft, syncService: service),
      ),
    );
    newDraft.add();
    await tester.pump();
    expect(find.text('●  변경된 사진 1장'), findsOneWidget);
  });

  testWidgets('valid non-preset interval is retained', (tester) async {
    useTallTestSurface(tester);
    final draft = PlaylistDraft(
      slides: [slideForWidget('a')],
      intervalMinutes: 2,
    );
    await tester.pumpWidget(
      MaterialApp(
        home: HomeScreen(
          draft: draft,
          syncService: EmptySynchronizationService(),
        ),
      ),
    );
    expect(find.text('2분마다'), findsOneWidget);
    expect(draft.intervalMinutes, 2);
  });
}

SlideItem slideForWidget(String id) => SlideItem(id: id, color: 0xff000000);

class _ControllerService implements SynchronizationService {
  _ControllerService(this.controller);
  final StreamController<SyncUpdate> controller;
  @override
  Stream<SyncUpdate> synchronize(SyncPlan plan) => controller.stream;
}
