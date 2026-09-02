import 'package:flutter/material.dart';

import '../state/playlist_models.dart';
import '../sync/fake_sync_service.dart';
import '../widgets/photo_placeholder.dart';
import 'photo_editor_screen.dart';
import 'sync_screen.dart';

class HomeScreen extends StatefulWidget {
  const HomeScreen({super.key, required this.draft, required this.syncService});
  final PlaylistDraft draft;
  final SynchronizationService syncService;
  @override
  State<HomeScreen> createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen> {
  @override
  void initState() {
    super.initState();
    widget.draft.addListener(_refresh);
  }

  @override
  void didUpdateWidget(covariant HomeScreen oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.draft == widget.draft) return;
    oldWidget.draft.removeListener(_refresh);
    widget.draft.addListener(_refresh);
  }

  @override
  void dispose() {
    widget.draft.removeListener(_refresh);
    super.dispose();
  }

  void _refresh() => setState(() {});

  Future<void> _edit({SlideItem? slide}) async {
    if (slide == null &&
        widget.draft.slides.length >= PlaylistDraft.maxSlides) {
      return;
    }
    final result = await Navigator.push<PhotoEditParameters>(
      context,
      MaterialPageRoute(builder: (_) => PhotoEditorScreen(slide: slide)),
    );
    if (result == null) return;
    if (slide == null) {
      widget.draft.add();
      widget.draft.edit(widget.draft.selectedId!, result);
    } else {
      widget.draft.edit(slide.id, result);
    }
  }

  @override
  Widget build(BuildContext context) => Scaffold(
    body: SafeArea(
      child: Center(
        child: ConstrainedBox(
          constraints: const BoxConstraints(maxWidth: 560),
          child: ListView(
            padding: const EdgeInsets.fromLTRB(24, 24, 24, 32),
            children: [
              Row(
                mainAxisAlignment: MainAxisAlignment.spaceBetween,
                children: [
                  const Text(
                    'Notua',
                    style: TextStyle(
                      fontFamily: 'serif',
                      fontSize: 30,
                      fontWeight: FontWeight.w700,
                    ),
                  ),
                  Container(
                    padding: const EdgeInsets.symmetric(
                      horizontal: 14,
                      vertical: 10,
                    ),
                    decoration: BoxDecoration(
                      border: Border.all(color: const Color(0xffdedbd3)),
                      borderRadius: BorderRadius.circular(16),
                    ),
                    child: const Row(
                      children: [
                        Icon(Icons.circle, size: 11, color: Color(0xff477a4a)),
                        SizedBox(width: 8),
                        Text('거실 액자 · 연결됨'),
                      ],
                    ),
                  ),
                ],
              ),
              const SizedBox(height: 24),
              if (widget.draft.selected case final selected?) ...[
                AspectRatio(
                  aspectRatio: 4 / 3,
                  child: Stack(
                    children: [
                      Positioned.fill(
                        child: PhotoPlaceholder(
                          color: selected.color,
                          parameters: selected.edit,
                        ),
                      ),
                      Positioned(
                        right: 12,
                        bottom: 12,
                        child: Row(
                          children: [
                            _CircleAction(
                              icon: Icons.edit_outlined,
                              label: '사진 편집',
                              onTap: () => _edit(slide: selected),
                            ),
                            const SizedBox(width: 8),
                            _CircleAction(
                              icon: Icons.delete_outline,
                              label: '사진 삭제',
                              onTap: () => widget.draft.remove(selected.id),
                            ),
                          ],
                        ),
                      ),
                    ],
                  ),
                ),
                const SizedBox(height: 20),
                SizedBox(
                  height: 124,
                  child: Row(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      for (var i = 0; i < widget.draft.slides.length; i++)
                        Expanded(
                          child: _Thumbnail(draft: widget.draft, index: i),
                        ),
                      if (widget.draft.slides.length < PlaylistDraft.maxSlides)
                        Expanded(
                          child: _AddCard(
                            number: widget.draft.slides.length + 1,
                            onTap: _edit,
                          ),
                        ),
                    ],
                  ),
                ),
              ] else
                _EmptyState(onAdd: _edit),
              const SizedBox(height: 20),
              _IntervalTile(
                value: widget.draft.intervalMinutes,
                onChanged: widget.draft.setInterval,
              ),
              const SizedBox(height: 22),
              Center(
                child: Text(
                  '●  변경된 사진 ${widget.draft.syncPlan.changedImageCount}장',
                  style: const TextStyle(color: Color(0xff656768)),
                ),
              ),
              const SizedBox(height: 22),
              FilledButton(
                onPressed:
                    widget.draft.slides.isNotEmpty &&
                        widget.draft.syncPlan.hasChanges
                    ? () => Navigator.push(
                        context,
                        MaterialPageRoute(
                          builder: (_) => SyncScreen(
                            draft: widget.draft,
                            service: widget.syncService,
                          ),
                        ),
                      )
                    : null,
                style: FilledButton.styleFrom(
                  minimumSize: const Size.fromHeight(60),
                  shape: RoundedRectangleBorder(
                    borderRadius: BorderRadius.circular(18),
                  ),
                ),
                child: const Text(
                  '액자에 적용하기',
                  style: TextStyle(fontSize: 17, fontWeight: FontWeight.w600),
                ),
              ),
            ],
          ),
        ),
      ),
    ),
  );
}

class _Thumbnail extends StatelessWidget {
  const _Thumbnail({required this.draft, required this.index});
  final PlaylistDraft draft;
  final int index;
  @override
  Widget build(BuildContext context) {
    final slide = draft.slides[index];
    final card = GestureDetector(
      onTap: () => draft.select(slide.id),
      child: Padding(
        padding: const EdgeInsets.only(right: 8),
        child: Column(
          children: [
            Expanded(
              child: Stack(
                clipBehavior: Clip.none,
                children: [
                  Positioned.fill(
                    child: DecoratedBox(
                      decoration: BoxDecoration(
                        border: Border.all(
                          color: draft.selectedId == slide.id
                              ? Theme.of(context).colorScheme.primary
                              : Colors.transparent,
                          width: 3,
                        ),
                        borderRadius: BorderRadius.circular(14),
                      ),
                      child: Padding(
                        padding: const EdgeInsets.all(2),
                        child: PhotoPlaceholder(color: slide.color, radius: 10),
                      ),
                    ),
                  ),
                  Positioned(
                    left: -3,
                    top: -5,
                    child: CircleAvatar(
                      radius: 11,
                      backgroundColor: const Color(0xff203f60),
                      child: Text(
                        '${index + 1}',
                        style: const TextStyle(
                          fontSize: 11,
                          color: Colors.white,
                        ),
                      ),
                    ),
                  ),
                ],
              ),
            ),
            const SizedBox(height: 4),
            const Icon(Icons.drag_handle, size: 19, color: Color(0xff777774)),
          ],
        ),
      ),
    );
    return DragTarget<int>(
      onAcceptWithDetails: (details) => draft.reorder(details.data, index),
      builder: (_, _, _) => LongPressDraggable<int>(
        data: index,
        feedback: SizedBox(
          width: 86,
          height: 100,
          child: Opacity(opacity: .8, child: card),
        ),
        child: card,
      ),
    );
  }
}

class _AddCard extends StatelessWidget {
  const _AddCard({required this.number, required this.onTap});
  final int number;
  final VoidCallback onTap;
  @override
  Widget build(BuildContext context) => Padding(
    padding: const EdgeInsets.only(right: 8),
    child: InkWell(
      onTap: onTap,
      borderRadius: BorderRadius.circular(14),
      child: Stack(
        children: [
          Positioned.fill(
            child: Container(
              decoration: BoxDecoration(
                border: Border.all(
                  color: const Color(0xffaaa8a2),
                  style: BorderStyle.solid,
                ),
                borderRadius: BorderRadius.circular(14),
              ),
              child: const Column(
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  Icon(Icons.add),
                  Text('사진 추가', style: TextStyle(fontSize: 12)),
                ],
              ),
            ),
          ),
          Positioned(
            left: 0,
            top: 0,
            child: CircleAvatar(
              radius: 11,
              backgroundColor: const Color(0xfffaf8f3),
              child: Text(
                '$number',
                style: const TextStyle(fontSize: 11, color: Colors.black),
              ),
            ),
          ),
        ],
      ),
    ),
  );
}

class _CircleAction extends StatelessWidget {
  const _CircleAction({
    required this.icon,
    required this.label,
    required this.onTap,
  });
  final IconData icon;
  final String label;
  final VoidCallback onTap;
  @override
  Widget build(BuildContext context) => IconButton.filledTonal(
    tooltip: label,
    onPressed: onTap,
    icon: Icon(icon),
  );
}

class _EmptyState extends StatelessWidget {
  const _EmptyState({required this.onAdd});
  final VoidCallback onAdd;
  @override
  Widget build(BuildContext context) => Container(
    height: 360,
    padding: const EdgeInsets.all(32),
    decoration: BoxDecoration(
      color: Colors.white,
      borderRadius: BorderRadius.circular(20),
      boxShadow: const [BoxShadow(color: Color(0x0d000000), blurRadius: 18)],
    ),
    child: Column(
      mainAxisAlignment: MainAxisAlignment.center,
      children: [
        const Icon(Icons.photo_outlined, size: 54, color: Color(0xff748292)),
        const SizedBox(height: 18),
        const Text(
          '첫 사진을 골라주세요',
          style: TextStyle(fontSize: 21, fontWeight: FontWeight.w700),
        ),
        const SizedBox(height: 8),
        const Text('좋아하는 순간을 최대 5장까지 담을 수 있어요', textAlign: TextAlign.center),
        const SizedBox(height: 24),
        FilledButton.icon(
          onPressed: onAdd,
          icon: const Icon(Icons.add),
          label: const Text('사진 추가'),
        ),
      ],
    ),
  );
}

class _IntervalTile extends StatelessWidget {
  const _IntervalTile({required this.value, required this.onChanged});
  final int value;
  final ValueChanged<int> onChanged;
  @override
  Widget build(BuildContext context) {
    const presets = [1, 5, 10, 30];
    final values = {...presets, value}.toList()..sort();
    return Container(
      decoration: BoxDecoration(
        color: Colors.white,
        borderRadius: BorderRadius.circular(18),
        boxShadow: const [BoxShadow(color: Color(0x0a000000), blurRadius: 14)],
      ),
      child: ListTile(
        title: const Text('슬라이드 전환'),
        trailing: DropdownButton<int>(
          value: value,
          underline: const SizedBox(),
          items: values
              .map((v) => DropdownMenuItem(value: v, child: Text('$v분마다')))
              .toList(),
          onChanged: (v) {
            if (v != null) onChanged(v);
          },
        ),
      ),
    );
  }
}
