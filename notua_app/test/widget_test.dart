import 'dart:async';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:image/image.dart' as img;
import 'package:notua_app/image_processing/image_pipeline.dart';
import 'package:notua_app/image_processing/photo_cache.dart';
import 'package:notua_app/image_processing/photo_picker_service.dart';
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

class ControlledImagePipeline extends ImagePipeline {
  final calls = <PhotoEditParameters>[];
  final completers = <Completer<ProcessedImage>>[];
  void Function(int callCount)? onProcessStarted;

  @override
  Future<ProcessedImage> process(Uint8List source, PhotoEditParameters edit,
      {bool fullSize = true}) {
    calls.add(edit);
    final completer = Completer<ProcessedImage>();
    completers.add(completer);
    onProcessStarted?.call(completers.length);
    return completer.future;
  }

  @override
  Future<Uint8List> originalPreview(Uint8List source, PhotoEditParameters edit) async =>
      previewBytes(0xffcccccc);
}

class FakePhotoPicker implements PhotoPickerService {
  FakePhotoPicker(this.photo, {this.lostPhoto});
  final SelectedPhoto photo;
  final SelectedPhoto? lostPhoto;
  @override
  Future<SelectedPhoto?> pickPhoto() async => photo;
  @override
  Future<SelectedPhoto?> retrieveLostPhoto() async => lostPhoto;
}

class MemoryPhotoCache implements PhotoCache {
  final files = <String, Uint8List>{};
  int _generation = 0;

  @override
  Uint8List? bytesForPath(String path) => files[path];

  @override
  Future<String> storeSource(
    String id,
    String extension,
    Uint8List bytes,
  ) async {
    final path = 'memory://$id-source.$extension';
    files[path] = Uint8List.fromList(bytes);
    return path;
  }

  @override
  Future<CachedOutputs> replaceOutputs(
    String id,
    Uint8List preview,
    Uint8List y8, {
    String? oldPreviewPath,
    String? oldBinPath,
  }) async {
    final generation = _generation++;
    final previewPath = 'memory://$id-$generation-preview.png';
    final binPath = 'memory://$id-$generation-frame.bin';
    files[previewPath] = Uint8List.fromList(preview);
    files[binPath] = Uint8List.fromList(y8);
    await deletePaths([oldPreviewPath, oldBinPath]);
    return CachedOutputs(previewPath: previewPath, binPath: binPath);
  }

  @override
  Future<void> deletePaths(Iterable<String?> paths) async {
    for (final path in paths.whereType<String>()) {
      files.remove(path);
    }
  }
}

class ImmediateImagePipeline extends ImagePipeline {
  @override
  Future<ProcessedImage> process(Uint8List source, PhotoEditParameters edit,
      {bool fullSize = true}) async => controlledResult(0xff336699, full: fullSize);
  @override
  Future<Uint8List> originalPreview(Uint8List source, PhotoEditParameters edit) async =>
      previewBytes(0xffdddddd);
}

class AdjustmentImagePipeline extends ImagePipeline {
  @override
  Future<ProcessedImage> process(Uint8List source, PhotoEditParameters edit,
      {bool fullSize = true}) async => controlledResult(
        edit.brightness > -18 ? 0xffeeeeee : 0xff111111,
        full: fullSize,
      );
  @override
  Future<Uint8List> originalPreview(Uint8List source, PhotoEditParameters edit) async =>
      previewBytes(0xff777777);
}

Uint8List previewBytes(int color) {
  final image = img.Image(width: 4, height: 3);
  final c = img.ColorRgba8((color >> 16) & 255, (color >> 8) & 255, color & 255, 255);
  img.fill(image, color: c);
  return Uint8List.fromList(img.encodePng(image));
}

ProcessedImage controlledResult(int color, {bool full = false}) => ProcessedImage(
  width: full ? 1600 : 400,
  height: full ? 1200 : 300,
  y8: Uint8List(full ? 1920000 : 120000),
  previewPng: previewBytes(color),
);

void useTallTestSurface(WidgetTester tester) {
  tester.view.physicalSize = const Size(800, 1400);
  tester.view.devicePixelRatio = 1;

  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);
}

Future<void> pumpAsyncWork(WidgetTester tester, {int times = 10}) async {
  for (var i = 0; i < times; i++) {
    await tester.pump(const Duration(milliseconds: 1));
  }
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
    await tester.pumpWidget(MaterialApp(home: PhotoEditorScreen(
      photo: SelectedPhoto(name: 'fixture.png', bytes: previewBytes(0xff224466), extension: 'png'),
      pipeline: ImmediateImagePipeline(),
    )));
    expect(find.text('사진 편집'), findsOneWidget);
    expect(find.text('밝기'), findsOneWidget);
    expect(find.text('대비'), findsOneWidget);
    expect(find.text('채도'), findsOneWidget);
    expect(find.text('슬라이드에 추가'), findsOneWidget);
  });

  testWidgets('each editor slider updates its real processing state', (tester) async {
    useTallTestSurface(tester);
    await tester.pumpWidget(MaterialApp(home: PhotoEditorScreen(
      photo: SelectedPhoto(name: 'fixture.png', bytes: previewBytes(0xff224466), extension: 'png'),
      pipeline: ImmediateImagePipeline(),
    )));
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

  testWidgets('stale async preview cannot replace the newest processed preview', (tester) async {
    useTallTestSurface(tester);
    final pipeline = ControlledImagePipeline();
    await tester.pumpWidget(MaterialApp(home: PhotoEditorScreen(
      photo: SelectedPhoto(name: 'fixture.png', bytes: previewBytes(0xff224466), extension: 'png'),
      pipeline: pipeline,
    )));
    await tester.pump();
    await tester.drag(find.byKey(PhotoEditorScreen.brightnessSliderKey), const Offset(80, 0));
    await tester.pump(const Duration(milliseconds: 200));
    await tester.drag(find.byKey(PhotoEditorScreen.contrastSliderKey), const Offset(60, 0));
    await tester.pump(const Duration(milliseconds: 200));
    expect(pipeline.completers, hasLength(1),
      reason: 'Only one isolate-backed process may be active.');
    pipeline.completers[0].complete(controlledResult(0xffff0000));
    await tester.pump();
    expect(pipeline.completers, hasLength(2),
      reason: 'Rapid updates are coalesced into one newest pending job.');
    final newest = controlledResult(0xff00ff00);
    pipeline.completers[1].complete(newest);
    await tester.pump();
    final shown = tester.widget<Image>(find.byKey(PhotoEditorScreen.previewKey));
    expect((shown.image as MemoryImage).bytes, newest.previewPng);
  });

  testWidgets('slider adjustment produces different processed preview bytes', (tester) async {
    useTallTestSurface(tester);
    await tester.pumpWidget(MaterialApp(home: PhotoEditorScreen(
      photo: SelectedPhoto(name: 'fixture.png', bytes: previewBytes(0xff224466), extension: 'png'),
      pipeline: AdjustmentImagePipeline(),
    )));
    await tester.pumpAndSettle();
    final before = (tester.widget<Image>(find.byKey(PhotoEditorScreen.previewKey)).image as MemoryImage).bytes;
    await tester.drag(find.byKey(PhotoEditorScreen.brightnessSliderKey), const Offset(100, 0));
    await tester.pumpAndSettle();
    final after = (tester.widget<Image>(find.byKey(PhotoEditorScreen.previewKey)).image as MemoryImage).bytes;
    expect(after, isNot(before));
  });

  testWidgets('fake selected photo becomes editor preview and real home thumbnail', (tester) async {
    useTallTestSurface(tester);
    final cache = MemoryPhotoCache();
    final draft = PlaylistDraft(slides: const []);
    await tester.pumpWidget(MaterialApp(home: HomeScreen(
      draft: draft,
      syncService: EmptySynchronizationService(),
      photoPicker: FakePhotoPicker(SelectedPhoto(name: 'fixture.png', bytes: previewBytes(0xff224466), extension: 'png')),
      photoCache: cache,
      imagePipeline: ImmediateImagePipeline(),
    )));
    await tester.tap(find.text('사진 추가'));
    await pumpAsyncWork(tester);
    expect(find.byKey(PhotoEditorScreen.previewKey), findsOneWidget);
    await tester.tap(find.text('원본 비교'));
    await tester.pump();
    expect(find.text('편집 보기'), findsOneWidget);
    await tester.tap(find.text('편집 보기'));
    await tester.tap(find.text('슬라이드에 추가'));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 400));
    await tester.pump();
    expect(draft.slides, hasLength(1));
    expect(find.text('사진 편집'), findsNothing);
    expect(find.byType(Image), findsWidgets);
    expect(cache.bytesForPath(draft.slides.single.previewPath!), isNotNull);
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
  });

  testWidgets('final conversion freezes controls and captures one edit value', (tester) async {
    useTallTestSurface(tester);
    final pipeline = ControlledImagePipeline();
    final finalStarted = Completer<void>();
    pipeline.onProcessStarted = (callCount) {
      if (callCount == 2) {
        finalStarted.complete();
      }
    };
    await tester.pumpWidget(MaterialApp(home: PhotoEditorScreen(
      photo: SelectedPhoto(name: 'fixture.png', bytes: previewBytes(0xff224466), extension: 'png'),
      pipeline: pipeline,
    )));
    await tester.pump();
    await tester.tap(find.text('슬라이드에 추가'));
    await tester.pump();
    final captured = pipeline.calls.last;
    expect(pipeline.completers, hasLength(1),
      reason: 'Save waits for the active preview rather than starting concurrently.');
    expect(tester.widget<Slider>(find.byKey(PhotoEditorScreen.brightnessSliderKey)).onChanged, isNull);
    expect(tester.widget<OutlinedButton>(find.widgetWithText(OutlinedButton, '왼쪽 회전')).onPressed, isNull);
    await tester.drag(find.byKey(PhotoEditorScreen.brightnessSliderKey), const Offset(100, 0));
    expect(pipeline.calls.last, same(captured));
    pipeline.completers.single.complete(controlledResult(0xff334455));
    await tester.pump();
    expect(finalStarted.isCompleted, isTrue);
    expect(pipeline.completers, hasLength(2));
    expect(pipeline.calls.last, same(captured));
    pipeline.completers.last.complete(controlledResult(0xff445566, full: true));
    await pumpAsyncWork(tester, times: 3);
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
