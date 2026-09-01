import 'dart:async';

import 'package:flutter/material.dart';

import '../state/playlist_models.dart';
import '../sync/fake_sync_service.dart';

class SyncScreen extends StatefulWidget {
  const SyncScreen({super.key, required this.draft, required this.service});

  final PlaylistDraft draft;
  final SynchronizationService service;

  @override
  State<SyncScreen> createState() => _SyncScreenState();
}

class _SyncScreenState extends State<SyncScreen> {
  late final SyncPlan plan = widget.draft.syncPlan;
  final Set<SyncStage> completedStages = {};
  StreamSubscription<SyncUpdate>? subscription;
  double progress = 0;
  Object? error;
  bool complete = false;

  @override
  void initState() {
    super.initState();
    _start();
  }

  void _start() {
    subscription?.cancel();
    setState(() {
      progress = 0;
      error = null;
      complete = false;
      completedStages.clear();
    });
    subscription = widget.service
        .synchronize(plan)
        .listen(
          (event) {
            if (!mounted) return;
            setState(() {
              progress = event.progress;
              completedStages.add(event.stage);
              if (event.progress == 1) {
                complete = true;
                widget.draft.markSynchronized();
              }
            });
          },
          onError: (Object value) {
            if (mounted) setState(() => error = value);
          },
        );
  }

  @override
  void dispose() {
    subscription?.cancel();
    super.dispose();
  }

  List<Widget> get _steps => [
    if (plan.uploads.isNotEmpty)
      _Step(
        icon: Icons.upload_outlined,
        label: '사진 전송',
        done: completedStages.contains(SyncStage.photos),
      ),
    if (plan.orderChanged || plan.deletedIds.isNotEmpty)
      _Step(
        icon: Icons.format_list_numbered,
        label: '순서 저장',
        done: completedStages.contains(SyncStage.order),
      ),
    if (plan.intervalChanged)
      _Step(
        icon: Icons.schedule,
        label: '전환 간격 저장',
        done: completedStages.contains(SyncStage.interval),
      ),
    _Step(
      icon: Icons.power_settings_new,
      label: '연결 종료',
      done: completedStages.contains(SyncStage.disconnect),
    ),
  ];

  @override
  Widget build(BuildContext context) => Scaffold(
    appBar: AppBar(title: const Text('액자에 적용'), centerTitle: true),
    body: SafeArea(
      child: LayoutBuilder(
        builder: (context, constraints) => SingleChildScrollView(
          padding: const EdgeInsets.all(24),
          child: ConstrainedBox(
            constraints: BoxConstraints(
              minHeight: (constraints.maxHeight - 48)
                  .clamp(0, double.infinity)
                  .toDouble(),
            ),
            child: Center(
              child: ConstrainedBox(
                constraints: const BoxConstraints(maxWidth: 520),
                child: Container(
                  padding: const EdgeInsets.all(28),
                  decoration: BoxDecoration(
                    color: Colors.white,
                    borderRadius: BorderRadius.circular(22),
                    boxShadow: const [
                      BoxShadow(color: Color(0x10000000), blurRadius: 20),
                    ],
                  ),
                  child: Column(
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      CircleAvatar(
                        radius: 34,
                        backgroundColor: error != null
                            ? const Color(0xffffe6df)
                            : const Color(0xff203f60),
                        child: Icon(
                          error != null
                              ? Icons.refresh
                              : complete
                              ? Icons.check
                              : Icons.sync,
                          color: error != null
                              ? Colors.deepOrange
                              : Colors.white,
                          size: 34,
                        ),
                      ),
                      const SizedBox(height: 18),
                      Text(
                        error != null
                            ? '적용하지 못했어요'
                            : complete
                            ? '액자에 적용했어요'
                            : '변경사항을 적용하고 있어요',
                        textAlign: TextAlign.center,
                        style: const TextStyle(
                          fontSize: 22,
                          fontWeight: FontWeight.w700,
                        ),
                      ),
                      const SizedBox(height: 8),
                      Text(
                        error != null
                            ? '변경사항은 그대로 보관했어요. 다시 시도해 주세요.'
                            : '변경된 사진 ${plan.changedImageCount}장과 설정을 업데이트해요',
                        textAlign: TextAlign.center,
                      ),
                      const SizedBox(height: 24),
                      LinearProgressIndicator(
                        value: progress,
                        borderRadius: BorderRadius.circular(6),
                      ),
                      const SizedBox(height: 7),
                      Align(
                        alignment: Alignment.centerRight,
                        child: Text('${(progress * 100).round()}%'),
                      ),
                      const SizedBox(height: 20),
                      ..._steps,
                      const SizedBox(height: 22),
                      FilledButton(
                        onPressed: error != null
                            ? _start
                            : complete
                            ? () => Navigator.pop(context)
                            : null,
                        style: FilledButton.styleFrom(
                          minimumSize: const Size.fromHeight(58),
                          shape: RoundedRectangleBorder(
                            borderRadius: BorderRadius.circular(17),
                          ),
                        ),
                        child: Text(error != null ? '다시 시도' : '완료'),
                      ),
                    ],
                  ),
                ),
              ),
            ),
          ),
        ),
      ),
    ),
  );
}

class _Step extends StatelessWidget {
  const _Step({required this.icon, required this.label, required this.done});

  final IconData icon;
  final String label;
  final bool done;

  @override
  Widget build(BuildContext context) => ListTile(
    contentPadding: EdgeInsets.zero,
    leading: CircleAvatar(
      backgroundColor: const Color(0xfff0f1eb),
      child: Icon(icon, color: const Color(0xff203f60)),
    ),
    title: Text(label),
    trailing: done
        ? const Icon(Icons.check, color: Color(0xff203f60))
        : const SizedBox(width: 18, height: 18),
  );
}
