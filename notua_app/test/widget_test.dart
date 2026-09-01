import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:notua_app/main.dart';
import 'package:notua_app/screens/photo_editor_screen.dart';
import 'package:notua_app/screens/sync_screen.dart';
import 'package:notua_app/state/playlist_models.dart';
import 'package:notua_app/sync/fake_sync_service.dart';

void main() {
  testWidgets('home presents polished slideshow labels', (tester) async {
    await tester.pumpWidget(const NotuaApp());
    expect(find.text('Notua'), findsOneWidget);
    expect(find.text('거실 액자 · 연결됨'), findsOneWidget);
    expect(find.text('슬라이드 전환'), findsOneWidget);
    expect(find.text('액자에 적용하기'), findsOneWidget);
  });
  testWidgets('empty state offers a photo action', (tester) async {
    await tester.pumpWidget(NotuaApp(initialDraft: PlaylistDraft(slides: const [])));
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
  testWidgets('sync screen displays its three stages', (tester) async {
    final draft = PlaylistDraft(slides: const []); draft.add();
    await tester.pumpWidget(MaterialApp(home: SyncScreen(draft: draft, service: FakeSynchronizationService(delay: const Duration(days: 1)))));
    expect(find.text('사진 전송'), findsOneWidget);
    expect(find.text('순서 저장'), findsOneWidget);
    expect(find.text('연결 종료'), findsOneWidget);
  });
}
