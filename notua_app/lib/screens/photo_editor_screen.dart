import 'package:flutter/material.dart';

import '../state/playlist_models.dart';
import '../widgets/photo_placeholder.dart';

class PhotoEditorScreen extends StatefulWidget {
  const PhotoEditorScreen({super.key, this.slide});
  final SlideItem? slide;
  @override State<PhotoEditorScreen> createState() => _PhotoEditorScreenState();
}

class _PhotoEditorScreenState extends State<PhotoEditorScreen> {
  late PhotoEditParameters value = widget.slide?.edit ?? const PhotoEditParameters();
  bool compare = false;
  void update(PhotoEditParameters next) => setState(() => value = next);
  @override Widget build(BuildContext context) => Scaffold(
    appBar: AppBar(title: const Text('사진 편집'), centerTitle: true),
    body: SafeArea(child: Center(child: ConstrainedBox(constraints: const BoxConstraints(maxWidth: 560), child: ListView(padding: const EdgeInsets.all(24), children: [
      AspectRatio(aspectRatio: 4 / 3, child: PhotoPlaceholder(color: widget.slide?.color ?? 0xff9a8881, parameters: compare ? const PhotoEditParameters(brightness: 0, contrast: 1) : value)),
      const SizedBox(height: 22),
      Row(children: [
        Expanded(child: _Tool(icon: Icons.rotate_left, text: '왼쪽 회전', onTap: () => update(value.copyWith(quarterTurns: (value.quarterTurns + 3) % 4)))),
        const SizedBox(width: 8), Expanded(child: _Tool(icon: Icons.rotate_right, text: '오른쪽 회전', onTap: () => update(value.copyWith(quarterTurns: (value.quarterTurns + 1) % 4)))),
        const SizedBox(width: 8), Expanded(child: _Tool(icon: Icons.crop, text: '자르기', onTap: () => ScaffoldMessenger.of(context).showSnackBar(const SnackBar(content: Text('자르기는 다음 단계에서 지원할 예정이에요.'))))),
        const SizedBox(width: 8), Expanded(child: _Tool(icon: Icons.compare, text: compare ? '편집 보기' : '원본 비교', onTap: () => setState(() => compare = !compare))),
      ]),
      const SizedBox(height: 22),
      _Adjust(label: '밝기', value: value.brightness, min: -60, max: 60, divisions: 60, display: value.brightness.toStringAsFixed(0), onChanged: (v) => update(value.copyWith(brightness: v))),
      _Adjust(label: '대비', value: value.contrast, min: .5, max: 2, divisions: 30, display: value.contrast.toStringAsFixed(2), onChanged: (v) => update(value.copyWith(contrast: v))),
      _Adjust(label: '채도', value: value.saturation, min: 0, max: 3, divisions: 60, display: value.saturation.toStringAsFixed(2), onChanged: (v) => update(value.copyWith(saturation: v))),
      const SizedBox(height: 22),
      Row(children: [
        OutlinedButton(onPressed: () => update(const PhotoEditParameters()), style: OutlinedButton.styleFrom(minimumSize: const Size(110, 58), shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(17))), child: const Text('초기화')),
        const SizedBox(width: 12), Expanded(child: FilledButton(onPressed: () => Navigator.pop(context, value), style: FilledButton.styleFrom(minimumSize: const Size.fromHeight(58), shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(17))), child: Text(widget.slide == null ? '슬라이드에 추가' : '변경사항 저장'))),
      ])
    ])))),
  );
}
class _Tool extends StatelessWidget { const _Tool({required this.icon, required this.text, required this.onTap}); final IconData icon; final String text; final VoidCallback onTap; @override Widget build(BuildContext context) => OutlinedButton(onPressed: onTap, style: OutlinedButton.styleFrom(padding: const EdgeInsets.symmetric(vertical: 14, horizontal: 3), shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(15))), child: Column(children: [Icon(icon), const SizedBox(height: 5), Text(text, style: const TextStyle(fontSize: 11))])); }
class _Adjust extends StatelessWidget { const _Adjust({required this.label, required this.value, required this.min, required this.max, required this.divisions, required this.display, required this.onChanged}); final String label, display; final double value, min, max; final int divisions; final ValueChanged<double> onChanged; @override Widget build(BuildContext context) => Row(children: [SizedBox(width: 42, child: Text(label, style: const TextStyle(fontWeight: FontWeight.w600))), Expanded(child: Slider(value: value.clamp(min, max).toDouble(), min: min, max: max, divisions: divisions, onChanged: onChanged)), SizedBox(width: 38, child: Text(display, textAlign: TextAlign.end))]); }
