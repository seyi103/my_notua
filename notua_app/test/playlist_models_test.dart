import 'package:flutter_test/flutter_test.dart';
import 'package:notua_app/state/playlist_models.dart';
import 'package:notua_app/sync/fake_sync_service.dart';

SlideItem slide(String id) => SlideItem(id: id, color: 0xff000000);
void main() {
  group('PlaylistDraft', () {
    test('enforces five-slide limit', () {
      final draft = PlaylistDraft(slides: [slide('1'), slide('2'), slide('3'), slide('4'), slide('5')]);
      expect(draft.add(), isFalse);
      expect(draft.slides, hasLength(5));
    });
    test('adds, selects, and removes slides', () {
      final draft = PlaylistDraft(slides: [slide('a')]);
      expect(draft.add(), isTrue);
      final added = draft.selectedId!;
      expect(draft.syncPlan.changedImageCount, 1);
      draft.remove(added);
      expect(draft.slides.map((e) => e.id), ['a']);
      expect(draft.selectedId, 'a');
    });
    test('reorders without image uploads', () {
      final draft = PlaylistDraft(slides: [slide('a'), slide('b'), slide('c')]);
      draft.reorder(0, 2);
      expect(draft.slides.map((e) => e.id), ['b', 'c', 'a']);
      expect(draft.syncPlan.orderChanged, isTrue);
      expect(draft.syncPlan.uploads, isEmpty);
    });
    test('selects an existing slide only', () {
      final draft = PlaylistDraft(slides: [slide('a'), slide('b')]);
      draft.select('b'); expect(draft.selectedId, 'b');
      draft.select('missing'); expect(draft.selectedId, 'b');
    });
    test('editing is an upload candidate and dirty count is accurate', () {
      final draft = PlaylistDraft(slides: [slide('a'), slide('b')]);
      draft.edit('b', const PhotoEditParameters(brightness: 4));
      expect(draft.syncPlan.changedImageCount, 1);
      expect(draft.syncPlan.uploads.single.id, 'b');
    });
    test('tracks interval independently', () {
      final draft = PlaylistDraft(slides: [slide('a')]);
      draft.setInterval(10);
      expect(draft.intervalMinutes, 10);
      expect(draft.syncPlan.intervalChanged, isTrue);
      expect(draft.syncPlan.uploads, isEmpty);
    });
    test('failed sync preserves draft', () async {
      final draft = PlaylistDraft(slides: [slide('a')]); draft.add();
      final before = draft.slides.map((e) => e.id).toList();
      await expectLater(
        FakeSynchronizationService(shouldFail: true, delay: Duration.zero).synchronize(draft.syncPlan),
        emitsInOrder([anything, anything, anything, anything, emitsError(isA<SyncException>())]),
      );
      expect(draft.slides.map((e) => e.id), before);
      expect(draft.syncPlan.changedImageCount, 1);
    });
  });
}
