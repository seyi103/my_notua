import 'dart:async';
import 'package:flutter/material.dart';
import '../state/playlist_models.dart';
import '../sync/fake_sync_service.dart';

class SyncScreen extends StatefulWidget { const SyncScreen({super.key, required this.draft, required this.service}); final PlaylistDraft draft; final SynchronizationService service; @override State<SyncScreen> createState() => _SyncScreenState(); }
class _SyncScreenState extends State<SyncScreen> {
  double progress = 0; SyncStage stage = SyncStage.photos; Object? error; bool complete = false; StreamSubscription<SyncUpdate>? sub;
  late final int changedImageCount = widget.draft.syncPlan.changedImageCount;
  @override void initState() { super.initState(); _start(); }
  void _start() { setState(() { progress = 0; error = null; complete = false; }); sub?.cancel(); sub = widget.service.synchronize(widget.draft.syncPlan).listen((event) { if (!mounted) return; setState(() { progress = event.progress; stage = event.stage; if (progress == 1) { complete = true; widget.draft.markSynchronized(); } }); }, onError: (Object value) { if (mounted) setState(() => error = value); }); }
  @override void dispose() { sub?.cancel(); super.dispose(); }
  @override Widget build(BuildContext context) => Scaffold(appBar: AppBar(title: const Text('액자에 적용'), centerTitle: true), body: SafeArea(child: Center(child: ConstrainedBox(constraints: const BoxConstraints(maxWidth: 520), child: Padding(padding: const EdgeInsets.all(24), child: Container(padding: const EdgeInsets.all(28), decoration: BoxDecoration(color: Colors.white, borderRadius: BorderRadius.circular(22), boxShadow: const [BoxShadow(color: Color(0x10000000), blurRadius: 20)]), child: Column(mainAxisSize: MainAxisSize.min, children: [
    CircleAvatar(radius: 34, backgroundColor: error != null ? const Color(0xffffe6df) : const Color(0xff203f60), child: Icon(error != null ? Icons.refresh : complete ? Icons.check : Icons.sync, color: error != null ? Colors.deepOrange : Colors.white, size: 34)),
    const SizedBox(height: 18), Text(error != null ? '적용하지 못했어요' : complete ? '액자에 적용했어요' : '사진을 적용하고 있어요', style: const TextStyle(fontSize: 22, fontWeight: FontWeight.w700)),
    const SizedBox(height: 8), Text(error != null ? '변경사항은 그대로 보관했어요. 다시 시도해 주세요.' : '변경된 사진 $changedImageCount장과 슬라이드 순서를 업데이트해요', textAlign: TextAlign.center),
    const SizedBox(height: 24), LinearProgressIndicator(value: progress, borderRadius: BorderRadius.circular(6)), const SizedBox(height: 7), Align(alignment: Alignment.centerRight, child: Text('${(progress * 100).round()}%')),
    const SizedBox(height: 20), _Step(icon: Icons.upload_outlined, label: '사진 전송', done: stage.index > 0 || complete), _Step(icon: Icons.format_list_numbered, label: '순서 저장', done: stage.index > 1 || complete), _Step(icon: Icons.power_settings_new, label: '연결 종료', done: complete),
    const SizedBox(height: 22), FilledButton(onPressed: error != null ? _start : complete ? () => Navigator.pop(context) : null, style: FilledButton.styleFrom(minimumSize: const Size.fromHeight(58), shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(17))), child: Text(error != null ? '다시 시도' : '완료')),
  ])))))));
}
class _Step extends StatelessWidget { const _Step({required this.icon, required this.label, required this.done}); final IconData icon; final String label; final bool done; @override Widget build(BuildContext context) => ListTile(contentPadding: EdgeInsets.zero, leading: CircleAvatar(backgroundColor: const Color(0xfff0f1eb), child: Icon(icon, color: const Color(0xff203f60))), title: Text(label), trailing: done ? const Icon(Icons.check, color: Color(0xff203f60)) : const SizedBox(width: 18, height: 18, child: CircularProgressIndicator(strokeWidth: 2)) ); }
