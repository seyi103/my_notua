import 'package:flutter/foundation.dart';

@immutable
class PhotoEditParameters {
  const PhotoEditParameters({
    this.brightness = -18,
    this.contrast = 0.5,
    this.saturation = 1,
    this.quarterTurns = 0,
  });

  final double brightness;
  final double contrast;
  final double saturation;
  final int quarterTurns;

  PhotoEditParameters copyWith({
    double? brightness,
    double? contrast,
    double? saturation,
    int? quarterTurns,
  }) => PhotoEditParameters(
    brightness: brightness ?? this.brightness,
    contrast: contrast ?? this.contrast,
    saturation: saturation ?? this.saturation,
    quarterTurns: quarterTurns ?? this.quarterTurns,
  );

  @override
  bool operator ==(Object other) =>
      other is PhotoEditParameters &&
      brightness == other.brightness &&
      contrast == other.contrast &&
      saturation == other.saturation &&
      quarterTurns == other.quarterTurns;

  @override
  int get hashCode =>
      Object.hash(brightness, contrast, saturation, quarterTurns);
}

@immutable
class SlideItem {
  const SlideItem({
    required this.id,
    required this.color,
    this.edit = const PhotoEditParameters(),
  });

  final String id;
  final int color;
  final PhotoEditParameters edit;

  SlideItem copyWith({PhotoEditParameters? edit}) =>
      SlideItem(id: id, color: color, edit: edit ?? this.edit);
}

@immutable
class DevicePlaylistSnapshot {
  const DevicePlaylistSnapshot({
    required this.slides,
    required this.intervalMinutes,
  });
  final List<SlideItem> slides;
  final int intervalMinutes;
}

@immutable
class SyncPlan {
  SyncPlan({
    required List<SlideItem> uploads,
    required List<String> deletedIds,
    required List<SlideItem> targetSlides,
    required List<String> targetSlideIds,
    required this.targetIntervalMinutes,
    required this.orderChanged,
    required this.intervalChanged,
  }) : uploads = List.unmodifiable(uploads),
       deletedIds = List.unmodifiable(deletedIds),
       targetSlides = List.unmodifiable(targetSlides),
       targetSlideIds = List.unmodifiable(targetSlideIds);
  final List<SlideItem> uploads;
  final List<String> deletedIds;
  final List<SlideItem> targetSlides;
  final List<String> targetSlideIds;
  final int targetIntervalMinutes;
  final bool orderChanged;
  final bool intervalChanged;
  int get changedImageCount => uploads.length;
  bool get hasChanges =>
      uploads.isNotEmpty ||
      deletedIds.isNotEmpty ||
      orderChanged ||
      intervalChanged;
}

class PlaylistDraft extends ChangeNotifier {
  static final RegExp _generatedIdPattern = RegExp(r'^new-(\d+)$');

  PlaylistDraft({required List<SlideItem> slides, int intervalMinutes = 5})
    : _slides = List.of(slides),
      _intervalMinutes = intervalMinutes,
      _snapshot = DevicePlaylistSnapshot(
        slides: List.of(slides),
        intervalMinutes: intervalMinutes,
      ),
      _selectedId = slides.isEmpty ? null : slides.first.id,
      _nextId = _deriveNextId(slides);

  factory PlaylistDraft.withMockData() => PlaylistDraft(
    slides: const [
      SlideItem(id: 'coast', color: 0xff7995a2),
      SlideItem(id: 'street', color: 0xffb88968),
      SlideItem(id: 'family', color: 0xffb5a28b),
      SlideItem(id: 'lake', color: 0xff6f8582),
    ],
  );

  static const maxSlides = 5;
  List<SlideItem> _slides;
  int _intervalMinutes;
  DevicePlaylistSnapshot _snapshot;
  String? _selectedId;
  int _nextId;
  SyncPlan? _pendingSyncPlan;

  static int _deriveNextId(List<SlideItem> slides) {
    var highest = 0;
    for (final slide in slides) {
      final match = _generatedIdPattern.firstMatch(slide.id);
      final value = match == null ? null : int.tryParse(match.group(1)!);
      if (value != null && value > highest) highest = value;
    }
    return highest + 1;
  }

  List<SlideItem> get slides => List.unmodifiable(_slides);
  int get intervalMinutes => _intervalMinutes;
  String? get selectedId => _selectedId;
  SlideItem? get selected =>
      _slides.where((item) => item.id == _selectedId).firstOrNull;
  SyncPlan get syncPlan {
    final oldById = {for (final slide in _snapshot.slides) slide.id: slide};
    final currentIds = _slides.map((e) => e.id).toList();
    final oldIds = _snapshot.slides.map((e) => e.id).toList();
    final uploads = _slides.where((slide) {
      final old = oldById[slide.id];
      return old == null || old.edit != slide.edit;
    }).toList();
    return SyncPlan(
      uploads: uploads,
      deletedIds: oldIds.where((id) => !currentIds.contains(id)).toList(),
      targetSlides: _slides,
      targetSlideIds: currentIds,
      targetIntervalMinutes: _intervalMinutes,
      orderChanged: !listEquals(oldIds, currentIds),
      intervalChanged: _intervalMinutes != _snapshot.intervalMinutes,
    );
  }

  bool add({int color = 0xff9a8881}) {
    if (_pendingSyncPlan != null) return false;
    if (_slides.length >= maxSlides) return false;
    final item = SlideItem(id: 'new-${_nextId++}', color: color);
    _slides.add(item);
    _selectedId = item.id;
    notifyListeners();
    return true;
  }

  void remove(String id) {
    if (_pendingSyncPlan != null) return;
    final index = _slides.indexWhere((item) => item.id == id);
    if (index < 0) return;
    _slides.removeAt(index);
    if (_selectedId == id) {
      _selectedId = _slides.isEmpty
          ? null
          : _slides[index.clamp(0, _slides.length - 1)].id;
    }
    notifyListeners();
  }

  void select(String id) {
    if (_pendingSyncPlan != null) return;
    if (_slides.any((item) => item.id == id)) {
      _selectedId = id;
      notifyListeners();
    }
  }

  void reorder(int oldIndex, int newIndex) {
    if (_pendingSyncPlan != null) return;
    if (oldIndex == newIndex ||
        oldIndex < 0 ||
        newIndex < 0 ||
        oldIndex >= _slides.length ||
        newIndex >= _slides.length) {
      return;
    }
    final item = _slides.removeAt(oldIndex);
    _slides.insert(newIndex, item);
    notifyListeners();
  }

  void edit(String id, PhotoEditParameters edit) {
    if (_pendingSyncPlan != null) return;
    final index = _slides.indexWhere((item) => item.id == id);
    if (index < 0) return;
    _slides[index] = _slides[index].copyWith(edit: edit);
    notifyListeners();
  }

  void setInterval(int minutes) {
    if (_pendingSyncPlan != null) return;
    if (_intervalMinutes == minutes) return;
    _intervalMinutes = minutes;
    notifyListeners();
  }

  SyncPlan beginSynchronization() {
    if (_slides.isEmpty) {
      throw StateError('An empty playlist cannot be synchronized.');
    }
    return _pendingSyncPlan ??= syncPlan;
  }

  void markSynchronized(SyncPlan plan) {
    if (!identical(plan, _pendingSyncPlan)) return;
    _snapshot = DevicePlaylistSnapshot(
      slides: List.of(plan.targetSlides),
      intervalMinutes: plan.targetIntervalMinutes,
    );
    _pendingSyncPlan = null;
    notifyListeners();
  }

  void abortSynchronization(SyncPlan plan) {
    if (!identical(plan, _pendingSyncPlan)) return;
    _pendingSyncPlan = null;
    notifyListeners();
  }
}

extension<T> on Iterable<T> {
  T? get firstOrNull => isEmpty ? null : first;
}
