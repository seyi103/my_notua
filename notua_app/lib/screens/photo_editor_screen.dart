import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter/material.dart';

import '../image_processing/image_pipeline.dart';
import '../image_processing/photo_picker_service.dart';
import '../state/playlist_models.dart';

class PhotoEditorResult {
  const PhotoEditorResult({required this.edit, required this.processed});
  final PhotoEditParameters edit;
  final ProcessedImage processed;
}

class PhotoEditorScreen extends StatefulWidget {
  const PhotoEditorScreen({super.key, this.slide, this.photo, this.pipeline});
  static const brightnessSliderKey = ValueKey<String>('brightness-slider');
  static const contrastSliderKey = ValueKey<String>('contrast-slider');
  static const saturationSliderKey = ValueKey<String>('saturation-slider');
  static const previewKey = ValueKey<String>('processed-preview');
  final SlideItem? slide;
  final SelectedPhoto? photo;
  final ImagePipeline? pipeline;
  @override
  State<PhotoEditorScreen> createState() => _PhotoEditorScreenState();
}

class _PhotoEditorScreenState extends State<PhotoEditorScreen> {
  late PhotoEditParameters value = widget.slide?.edit ?? const PhotoEditParameters();
  late final Uint8List source = widget.photo?.bytes ?? widget.slide?.sourceBytes ??
      base64Decode('iVBORw0KGgoAAAANSUhEUgAAAAQAAAADCAIAAAA7l2cLAAAADElEQVR4nGP4z4AATAAAMQUB/2VwxAAAAABJRU5ErkJggg==');
  late final ImagePipeline pipeline = widget.pipeline ?? ImagePipeline();
  Uint8List? preview;
  Timer? _debounce;
  int _generation = 0;
  bool saving = false;

  @override
  void initState() {
    super.initState();
    preview = widget.slide?.previewPng;
    _schedulePreview(immediate: preview == null);
  }

  @override
  void dispose() {
    _debounce?.cancel();
    _generation++;
    super.dispose();
  }

  void update(PhotoEditParameters next) {
    setState(() => value = next);
    _schedulePreview();
  }

  void _schedulePreview({bool immediate = false}) {
    _debounce?.cancel();
    final token = ++_generation;
    _debounce = Timer(immediate ? Duration.zero : const Duration(milliseconds: 180), () async {
      try {
        final result = await pipeline.process(source, value, fullSize: false);
        if (!mounted || token != _generation) return;
        setState(() => preview = result.previewPng);
      } on Object {
        if (mounted && token == _generation) {
          ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('이미지를 처리할 수 없어요.')));
        }
      }
    });
  }

  Future<void> _crop() async {
    var scale = value.crop.width;
    var horizontal = value.crop.left;
    var vertical = value.crop.top;
    final next = await showDialog<NormalizedCrop>(context: context, builder: (context) => StatefulBuilder(
      builder: (context, setDialogState) => AlertDialog(
        title: const Text('4:3 자르기'),
        content: Column(mainAxisSize: MainAxisSize.min, children: [
          const Text('프레임 크기와 위치를 조절하세요. 출력 비율은 4:3으로 고정됩니다.'),
          Slider(value: scale.clamp(.25, 1).toDouble(), min: .25, max: 1, onChanged: (v) => setDialogState(() => scale = v)),
          Slider(value: horizontal.clamp(0, 1 - scale).toDouble(), max: (1 - scale).clamp(.001, 1).toDouble(), onChanged: (v) => setDialogState(() => horizontal = v)),
          Slider(value: vertical.clamp(0, 1 - scale).toDouble(), max: (1 - scale).clamp(.001, 1).toDouble(), onChanged: (v) => setDialogState(() => vertical = v)),
        ]),
        actions: [TextButton(onPressed: () => Navigator.pop(context), child: const Text('취소')),
          FilledButton(onPressed: () => Navigator.pop(context, NormalizedCrop(left: horizontal, top: vertical, width: scale, height: scale)), child: const Text('적용'))],
      ),
    ));
    if (next != null) update(value.copyWith(crop: next));
  }

  Future<void> _save() async {
    setState(() => saving = true);
    try {
      final result = await pipeline.process(source, value);
      if (mounted) Navigator.pop(context, PhotoEditorResult(edit: value, processed: result));
    } on Object catch (error) {
      if (mounted) ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('$error')));
      if (mounted) setState(() => saving = false);
    }
  }

  @override
  Widget build(BuildContext context) => Scaffold(
    appBar: AppBar(title: const Text('사진 편집'), centerTitle: true),
    body: SafeArea(child: Center(child: ConstrainedBox(constraints: const BoxConstraints(maxWidth: 560), child: ListView(
      padding: const EdgeInsets.all(24), children: [
        AspectRatio(aspectRatio: 4 / 3, child: ClipRRect(borderRadius: BorderRadius.circular(20), child: preview == null
          ? const Center(child: CircularProgressIndicator())
          : Image.memory(preview!, key: PhotoEditorScreen.previewKey, fit: BoxFit.cover, gaplessPlayback: true))),
        const SizedBox(height: 22),
        Row(children: [
          Expanded(child: _Tool(icon: Icons.rotate_left, text: '왼쪽 회전', onTap: () => update(value.copyWith(quarterTurns: (value.quarterTurns + 3) % 4)))),
          const SizedBox(width: 8),
          Expanded(child: _Tool(icon: Icons.rotate_right, text: '오른쪽 회전', onTap: () => update(value.copyWith(quarterTurns: (value.quarterTurns + 1) % 4)))),
          const SizedBox(width: 8),
          Expanded(child: _Tool(icon: Icons.crop, text: '4:3 자르기', onTap: _crop)),
        ]),
        const SizedBox(height: 22),
        _Adjust(sliderKey: PhotoEditorScreen.brightnessSliderKey, label: '밝기', value: value.brightness, min: -60, max: 60, divisions: 60, display: value.brightness.toStringAsFixed(0), onChanged: (v) => update(value.copyWith(brightness: v))),
        _Adjust(sliderKey: PhotoEditorScreen.contrastSliderKey, label: '대비', value: value.contrast, min: .5, max: 2, divisions: 30, display: value.contrast.toStringAsFixed(2), onChanged: (v) => update(value.copyWith(contrast: v))),
        _Adjust(sliderKey: PhotoEditorScreen.saturationSliderKey, label: '채도', value: value.saturation, min: 0, max: 3, divisions: 60, display: value.saturation.toStringAsFixed(2), onChanged: (v) => update(value.copyWith(saturation: v))),
        const SizedBox(height: 22),
        Row(children: [OutlinedButton(onPressed: saving ? null : () => update(const PhotoEditParameters()), child: const Text('초기화')),
          const SizedBox(width: 12), Expanded(child: FilledButton(onPressed: saving ? null : _save, child: Text(saving ? '처리 중…' : widget.slide == null ? '슬라이드에 추가' : '변경사항 저장')))]),
      ],
    )))),
  );
}

class _Tool extends StatelessWidget {
  const _Tool({required this.icon, required this.text, required this.onTap});
  final IconData icon; final String text; final VoidCallback onTap;
  @override Widget build(BuildContext context) => OutlinedButton(onPressed: onTap, child: Column(children: [Icon(icon), Text(text, style: const TextStyle(fontSize: 11))]));
}

class _Adjust extends StatelessWidget {
  const _Adjust({required this.sliderKey, required this.label, required this.value, required this.min, required this.max, required this.divisions, required this.display, required this.onChanged});
  final Key sliderKey; final String label, display; final double value, min, max; final int divisions; final ValueChanged<double> onChanged;
  @override Widget build(BuildContext context) => Row(children: [SizedBox(width: 42, child: Text(label)), Expanded(child: Slider(key: sliderKey, value: value.clamp(min, max).toDouble(), min: min, max: max, divisions: divisions, onChanged: onChanged)), SizedBox(width: 38, child: Text(display))]);
}
