import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter/material.dart';

import '../image_processing/image_pipeline.dart';
import '../image_processing/photo_picker_service.dart';
import '../state/playlist_models.dart';
import 'visual_crop_screen.dart';

Uint8List _editorSource(PhotoEditorScreen widget) {
  if (widget.photo != null) return widget.photo!.bytes;
  final path = widget.sourcePath ?? widget.slide?.sourcePath;
  if (path != null) return File(path).readAsBytesSync();
  throw StateError('PhotoEditorScreen requires a selected or cached source image.');
}

class PhotoEditorResult {
  const PhotoEditorResult({required this.edit, required this.processed});
  final PhotoEditParameters edit;
  final ProcessedImage processed;
}

class PhotoEditorScreen extends StatefulWidget {
  const PhotoEditorScreen({super.key, this.slide, this.photo, this.sourcePath, this.pipeline});
  static const brightnessSliderKey = ValueKey<String>('brightness-slider');
  static const contrastSliderKey = ValueKey<String>('contrast-slider');
  static const saturationSliderKey = ValueKey<String>('saturation-slider');
  static const previewKey = ValueKey<String>('processed-preview');
  final SlideItem? slide;
  final SelectedPhoto? photo;
  final String? sourcePath;
  final ImagePipeline? pipeline;
  @override
  State<PhotoEditorScreen> createState() => _PhotoEditorScreenState();
}

class _PhotoEditorScreenState extends State<PhotoEditorScreen> {
  late PhotoEditParameters value = widget.slide?.edit ?? const PhotoEditParameters();
  late final Uint8List source = _editorSource(widget);
  late final ImagePipeline pipeline = widget.pipeline ?? ImagePipeline();
  Uint8List? preview;
  Uint8List? originalPreview;
  Timer? _debounce;
  Future<void>? _previewWork;
  _PendingPreview? _pendingPreview;
  int _generation = 0;
  bool saving = false;
  bool compare = false;
  String? processingError;

  @override
  void initState() {
    super.initState();
    if (value.crop == const NormalizedCrop()) {
      value = value.copyWith(crop: ImagePipeline.defaultCrop(source, value.quarterTurns));
    }
    if (widget.slide?.previewPath case final path?) {
      preview = File(path).readAsBytesSync();
    }
    _schedulePreview(immediate: preview == null);
  }

  @override
  void dispose() {
    _debounce?.cancel();
    _generation++;
    super.dispose();
  }

  void update(PhotoEditParameters next) {
    if (saving) return;
    setState(() => value = next);
    _schedulePreview();
  }

  void _schedulePreview({bool immediate = false}) {
    _debounce?.cancel();
    final token = ++_generation;
    final captured = value;
    _debounce = Timer(
      immediate ? Duration.zero : const Duration(milliseconds: 180),
      () {
        _pendingPreview = _PendingPreview(token, captured);
        _startPreviewWorker();
      },
    );
  }

  void _startPreviewWorker() {
    if (_previewWork != null || saving) return;
    late final Future<void> work;
    work = _drainPreviewQueue();
    _previewWork = work;
    work.whenComplete(() {
      if (!identical(_previewWork, work)) return;
      _previewWork = null;
      if (_pendingPreview != null && mounted && !saving) {
        _startPreviewWorker();
      }
    });
  }

  Future<void> _drainPreviewQueue() async {
    while (true) {
      final request = _pendingPreview;
      if (request == null) return;
      _pendingPreview = null;
      try {
        final result = await pipeline.process(
          source,
          request.edit,
          fullSize: false,
        );
        final original = await pipeline.originalPreview(source, request.edit);
        if (!mounted || request.token != _generation) continue;
        setState(() {
          preview = result.previewPng;
          originalPreview = original;
          processingError = null;
        });
      } on Object {
        if (mounted && request.token == _generation) {
          setState(() => processingError = '이미지를 처리할 수 없어요. 다른 JPEG 또는 PNG를 선택해 주세요.');
        }
      }
    }
  }

  Future<void> _crop() async {
    if (saving) return;
    final next = await Navigator.push<NormalizedCrop>(context,
      MaterialPageRoute(builder: (_) => VisualCropScreen(source: source, edit: value)));
    if (next != null) update(value.copyWith(crop: next));
  }

  Future<void> _save() async {
    _debounce?.cancel();
    final captured = value;
    ++_generation;
    _pendingPreview = null;
    setState(() => saving = true);
    try {
      await _previewWork;
      final result = await pipeline.process(source, captured);
      if (mounted) Navigator.pop(context, PhotoEditorResult(edit: captured, processed: result));
    } on Object catch (error) {
      if (mounted) ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('$error')));
      if (mounted) setState(() {
        saving = false;
        processingError = '최종 이미지를 만들지 못했어요: $error';
      });
    }
  }

  @override
  Widget build(BuildContext context) => Scaffold(
    appBar: AppBar(title: const Text('사진 편집'), centerTitle: true),
    body: SafeArea(child: Center(child: ConstrainedBox(constraints: const BoxConstraints(maxWidth: 560), child: ListView(
      padding: const EdgeInsets.all(24), children: [
        AspectRatio(aspectRatio: 4 / 3, child: ClipRRect(borderRadius: BorderRadius.circular(20), child: preview == null
          ? const Center(child: CircularProgressIndicator())
          : Image.memory(compare && originalPreview != null ? originalPreview! : preview!, key: PhotoEditorScreen.previewKey, fit: BoxFit.cover, gaplessPlayback: true,
              errorBuilder: (_, _, _) => const Center(child: Text('이미지 미리보기를 표시할 수 없어요.'))))),
        if (processingError case final error?) ...[
          const SizedBox(height: 10),
          Text(error, style: TextStyle(color: Theme.of(context).colorScheme.error), textAlign: TextAlign.center),
        ],
        const SizedBox(height: 22),
        Row(children: [
          Expanded(child: _Tool(icon: Icons.rotate_left, text: '왼쪽 회전', enabled: !saving, onTap: () { final turns = (value.quarterTurns + 3) % 4; update(value.copyWith(quarterTurns: turns, crop: ImagePipeline.defaultCrop(source, turns))); })),
          const SizedBox(width: 8),
          Expanded(child: _Tool(icon: Icons.rotate_right, text: '오른쪽 회전', enabled: !saving, onTap: () { final turns = (value.quarterTurns + 1) % 4; update(value.copyWith(quarterTurns: turns, crop: ImagePipeline.defaultCrop(source, turns))); })),
          const SizedBox(width: 8),
          Expanded(child: _Tool(icon: Icons.crop, text: '4:3 자르기', enabled: !saving, onTap: _crop)),
          const SizedBox(width: 8),
          Expanded(child: _Tool(icon: Icons.compare, text: compare ? '편집 보기' : '원본 비교', enabled: !saving,
            onTap: () => setState(() => compare = !compare))),
        ]),
        const SizedBox(height: 22),
        _Adjust(sliderKey: PhotoEditorScreen.brightnessSliderKey, label: '밝기', value: value.brightness, min: -60, max: 60, divisions: 60, display: value.brightness.toStringAsFixed(0), onChanged: saving ? null : (v) => update(value.copyWith(brightness: v))),
        _Adjust(sliderKey: PhotoEditorScreen.contrastSliderKey, label: '대비', value: value.contrast, min: .5, max: 2, divisions: 30, display: value.contrast.toStringAsFixed(2), onChanged: saving ? null : (v) => update(value.copyWith(contrast: v))),
        _Adjust(sliderKey: PhotoEditorScreen.saturationSliderKey, label: '채도', value: value.saturation, min: 0, max: 3, divisions: 60, display: value.saturation.toStringAsFixed(2), onChanged: saving ? null : (v) => update(value.copyWith(saturation: v))),
        const SizedBox(height: 22),
        Row(children: [OutlinedButton(
          onPressed: saving ? null : () => update(PhotoEditParameters(
            crop: ImagePipeline.defaultCrop(source, 0))),
          style: OutlinedButton.styleFrom(minimumSize: const Size(110, 58),
            shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(17))),
          child: const Text('초기화')),
          const SizedBox(width: 12), Expanded(child: FilledButton(
            onPressed: saving ? null : _save,
            style: FilledButton.styleFrom(minimumSize: const Size.fromHeight(58),
              shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(17))),
            child: saving
              ? const Row(mainAxisAlignment: MainAxisAlignment.center, children: [
                  SizedBox.square(dimension: 18, child: CircularProgressIndicator(strokeWidth: 2)),
                  SizedBox(width: 10), Text('처리 중…'),
                ])
              : Text(widget.slide == null ? '슬라이드에 추가' : '변경사항 저장')))]),
      ],
    )))),
  );
}

class _PendingPreview {
  const _PendingPreview(this.token, this.edit);
  final int token;
  final PhotoEditParameters edit;
}

class _Tool extends StatelessWidget {
  const _Tool({required this.icon, required this.text, required this.onTap, this.enabled = true});
  final IconData icon; final String text; final VoidCallback onTap; final bool enabled;
  @override Widget build(BuildContext context) => OutlinedButton(
    onPressed: enabled ? onTap : null,
    style: OutlinedButton.styleFrom(minimumSize: const Size.fromHeight(58),
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(15))),
    child: Column(children: [Icon(icon), const SizedBox(height: 5), Text(text, style: const TextStyle(fontSize: 11))]));
}

class _Adjust extends StatelessWidget {
  const _Adjust({required this.sliderKey, required this.label, required this.value, required this.min, required this.max, required this.divisions, required this.display, required this.onChanged});
  final Key sliderKey; final String label, display; final double value, min, max; final int divisions; final ValueChanged<double>? onChanged;
  @override Widget build(BuildContext context) => Row(children: [SizedBox(width: 42, child: Text(label)), Expanded(child: Slider(key: sliderKey, value: value.clamp(min, max).toDouble(), min: min, max: max, divisions: divisions, onChanged: onChanged)), SizedBox(width: 38, child: Text(display))]);
}
